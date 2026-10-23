#include "scene_resource_capture.h"
#include "route_effect_trace.h"
#include "scene_payload_store.h"
#include "scene_target_readback.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rex/crypto/sha256.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{

std::atomic<void*> sceneCaptureSession{ nullptr };
std::atomic<void*> sceneCoverageSession{ nullptr };
static_assert(std::atomic<void*>::is_always_lock_free);

struct SceneWriterState
{
    toml::table                      metadata;
    std::vector<SceneGuestRange>     destinations;
    std::uint32_t                    directRegionPair = 0, directRegionRows = 0, directRegionRowBytes = 0;
    std::vector<ScenePayloadBinding> sourcePayloads;
    bool                             payloadOnly   = false;
    bool                             regionPending = false, regionOutOfWindow = false;
    bool                             preArmRegions = false;
    std::uint32_t                    regionRecord = 0, regionPitch = 0, regionAddress = 0;
    toml::array                      regions;
};

struct ScenePayloadCall
{
    std::uint64_t             frame = 0, event = 0, submission = 0, parent = 0;
    std::uint32_t             graphics = 0;
    std::vector<std::uint8_t> before;
    bool                      inWindow = false;
};

namespace
{
namespace xenos                                      = rex::graphics::xenos;
namespace util                                       = rex::graphics::texture_util;
constexpr std::uint32_t                     kMaxRead = 64 * 1024 * 1024, kMaxCode = 64 * 1024, kMaxLiteralTable = 4096;
constexpr std::uint64_t                     kMaxEvents = 2000000, kMaxDraws = 16384;
constexpr std::uint64_t                     kMaxOutputBytes  = 4ULL * 1024 * 1024 * 1024;
constexpr std::size_t                       kMaxWriterRanges = 262144;
thread_local std::vector<SceneWriterState*> writerStack;
thread_local std::vector<ScenePayloadCall*> payloadStack;
#if defined(REREVVED_SCENE_CAPTURE_TEST)
SceneTestMemoryReader testMemoryReader = nullptr;
#endif

std::uint32_t word(std::span<const std::uint8_t> b, std::size_t n)
{
    if (n > b.size() || b.size() - n < 4)
        throw std::runtime_error("scene_word_extent");
    return (std::uint32_t(b[n]) << 24) | (std::uint32_t(b[n + 1]) << 16) | (std::uint32_t(b[n + 2]) << 8) | b[n + 3];
}

std::span<const std::uint8_t> memory(std::uint64_t address, std::uint64_t size, bool physical = false)
{
    if (!size)
        return {};
    const auto ceiling = physical ? 0x20000000ULL : 0x100000000ULL;
    if (size > kMaxRead || address >= ceiling || size > ceiling - address)
        throw std::runtime_error("scene_memory_extent");
#if defined(REREVVED_SCENE_CAPTURE_TEST)
    if (testMemoryReader)
        return testMemoryReader(address, size, physical);
#endif
    auto*      m    = REX_KERNEL_MEMORY();
    auto*      heap = physical ? m->GetPhysicalHeap() : m->LookupHeap(static_cast<std::uint32_t>(address));
    const auto last = static_cast<std::uint32_t>(address + size - 1);
    if (!heap || (!physical && m->LookupHeap(last) != heap) ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(static_cast<std::uint32_t>(address), last)) &
          static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        throw std::runtime_error("scene_memory_unreadable");
    return { physical ? m->TranslatePhysical<const std::uint8_t*>(static_cast<std::uint32_t>(address)) : m->TranslateVirtual<const std::uint8_t*>(static_cast<std::uint32_t>(address)), static_cast<std::size_t>(size) };
}

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string    text;
    text.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text;
}

toml::array words(std::span<const std::uint8_t> b)
{
    toml::array a;
    for (std::size_t n = 0; n + 4 <= b.size(); n += 4)
        a.push_back(word(b, n));
    return a;
}

SceneFetch fetch(std::span<const std::uint8_t> b, std::size_t offset)
{
    SceneFetch w{};
    for (std::size_t n = 0; n < 6; ++n)
        w[n] = word(b, offset + n * 4);
    return w;
}

toml::table texture(const SceneFetch& w)
{
    const auto  t = DecodeSceneFetch(w);
    toml::array raw;
    for (auto v : w)
        raw.push_back(v);
    toml::table table{ { "words", raw }, { "type", t.type }, { "base", t.base }, { "mip_base", t.mipBase }, { "format", t.format }, { "width", t.width }, { "height", t.height }, { "depth", t.depth }, { "pitch", t.pitch }, { "dimension", t.dimension }, { "tiled", t.tiled }, { "endian", t.endian }, { "swizzle", t.swizzle }, { "min_mip", t.minMip }, { "max_mip", t.maxMip }, { "packed_mips", t.packedMips }, { "signs", t.signs }, { "num_format", t.numFormat }, { "stacked", t.stacked } };
    table.insert("exp_adjust", t.expAdjust);
    // Reject unsupported descriptors before calling SDK helpers with assertions.
    if (t.type == 2 && t.base < 0x20000000 && t.mipBase < 0x20000000 &&
        t.width <= 16384 && t.height <= 8192 && t.depth <= 1024 &&
        !(t.dimension == 0 && (t.tiled || t.packedMips || t.stacked)) &&
        !(t.dimension == 2 && t.stacked) && !(t.dimension == 3 && (t.stacked || t.depth != 6)))
    {
        xenos::xe_gpu_texture_fetch_t f{};
        std::memcpy(&f, w.data(), sizeof(f));
        std::uint32_t width, height, depth, base, mips, min, max;
        util::GetSubresourcesFromFetchConstant(f, &width, &height, &depth, &base, &mips, &min, &max);
        const auto layout = util::GetGuestTextureLayout(f.dimension, f.pitch, width + 1, height + 1, depth + 1, f.tiled, f.format, f.packed_mips, base != 0, max);
        table.insert("base_bytes", base ? layout.base.level_data_extent_bytes : 0);
        table.insert("mip_bytes", mips ? layout.mips_total_extent_bytes : 0);
        table.insert("range_source", "SDK texture_util::GetGuestTextureLayout (storage extent; tiled padding)");
    }
    return table;
}

void header(toml::table& t, std::uint32_t address, std::size_t size = 0x34)
{
    t.insert("object", address);
    if (!address)
        return;
    const auto b = memory(address, size);
    t.insert("header_hex", hex(b));
    if (size == 0x34)
    {
        auto        w = fetch(b, 0x1C);
        toml::array raw;
        for (auto v : w)
            raw.push_back(v);
        // RVA-F-0031 descriptorMerge; Resolve applies the same physical alias.
        const auto primary   = (w[1] & 0x1FFFFFFFU) + ((((w[1] >> 20) & 0xFFF) + 0x200) & 0x1000);
        const auto secondary = (w[5] & 0x1FFFFE00U) + ((((w[5] >> 20) & 0xFFF) + 0x200) & 0x1000);
        w[1]                 = (primary & 0xFFFFF000U) | (w[1] & 0xFFF);
        w[5]                 = (secondary & 0xFFFFF000U) | (w[5] & 0xFFF);
        auto decoded         = texture(w);
        decoded.insert("raw_object_descriptor", std::move(raw));
        decoded.insert("address_source", "sub_8269AF10 physical alias; sub_8269E520 8269E6DC..8269E714");
        t.insert("texture", std::move(decoded));
    }
}

bool drawEntry(std::uint32_t entry)
{
    return entry == 0x826A3568 || entry == 0x826A3000 || entry == 0x826A39F8 || entry == 0x826A3608 || entry == 0x826A3048 || entry == 0x826A2B58;
}

bool createEntry(std::uint32_t entry)
{
    return entry == 0x82533DB8 || entry == 0x8269AB90 || entry == 0x82302DC8 || entry == 0x826B7140 || entry == 0x825184D0;
}

bool writerEntry(std::uint32_t e)
{
    return e == 0x826B5F30 || e == 0x82521558 || e == 0x82305648 ||
           e == 0x82C69850 || e == 0x827F8550;
}

bool writerCapacityFailure(std::string_view reason)
{
    return reason.starts_with("scene_writer_") && reason.ends_with("_capacity");
}

// SDK xmemory.cpp physical mapping: A/C aliases start at physical zero;
// E aliases start at physical 0x1000. Low virtual addresses are not physical.
std::uint64_t physicalAddress(std::uint32_t address)
{
    return (address & 0x1FFFFFFFULL) + (address >= 0xE0000000U ? 0x1000 : 0);
}

void destination(SceneWriterState& w, std::uint64_t address, std::uint64_t size)
{
    if (!size)
        return;
    if (size > kMaxRead || address > UINT32_MAX || size > 0x100000000ULL - address)
        throw std::runtime_error("scene_writer_destination_extent");
    if (w.destinations.size() >= kMaxWriterRanges)
        throw std::runtime_error("scene_writer_range_capacity");
    w.destinations.push_back({ static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size) });
}

// Source bytes are corroboration only. Path A reads 0xC/0xD physical aliases
// that no virtual heap reports, so fall back to the physical heap and record a
// source-only failure without discarding the writer's destination evidence.
void source(toml::table& t, std::uint64_t address, std::uint64_t size)
{
    t.insert("source", static_cast<std::int64_t>(address));
    t.insert("source_bytes", static_cast<std::int64_t>(size));
    try
    {
        t.insert("source_sha256", digest(memory(address, size)));
        return;
    }
    catch (const std::exception& e)
    {
        if (address < 0xA0000000ULL || address > 0xFFFFFFFFULL)
        {
            t.insert("source_read_error", e.what());
            return;
        }
    }
    try
    {
        t.insert("source_sha256", digest(memory(physicalAddress(static_cast<std::uint32_t>(address)), size, true)));
        t.insert("source_physical", true);
    }
    catch (const std::exception& e)
    {
        t.insert("source_read_error", e.what());
    }
}

void linearDestination(SceneWriterState& w, std::uint32_t address, std::uint32_t pitch, std::uint32_t rows, std::uint32_t rowBytes)
{
    if (rows > 8192)
        throw std::runtime_error("scene_writer_row_capacity");
    for (std::uint64_t row = 0; row < rows; ++row)
        destination(w, std::uint64_t(address) + row * pitch, rowBytes);
}

toml::table arguments(const PPCContext& c);

void decodeWriter(SceneWriterState& w, std::uint32_t e, const PPCContext& c)
{
    auto& t = w.metadata;
    t       = arguments(c);
    t.insert("kind", "cpu_writer_begin");
    t.insert("caller_lr", std::to_string(c.lr));
    std::string path = e == 0x826B5F30 ? "A" : e == 0x82521558 ? "B"
                                           : e == 0x82305648   ? "C"
                                           : e == 0x82C69850   ? "D"
                                                               : "copy";
    t.insert("writer_path", path);
    if (e == 0x827F8550)
    {
        t.insert("provenance", "sub_827F8550 827F8550..827F85D0; r3 destination, r4 source, r5 bytes");
        destination(w, c.r3.u32, c.r5.u32);
        source(t, c.r4.u32, c.r5.u32);
    }
    else if (e == 0x826B5F30)
    {
        const auto                   stack        = memory(std::uint64_t(c.r1.u32) + 0x54, 20);
        const auto                   sourceFormat = word(stack, 0);
        const auto                   rectangle    = word(stack, 8);
        std::array<std::uint32_t, 4> rect{ 0, 0, c.r5.u32, c.r6.u32 };
        std::array<std::uint32_t, 2> origin{};
        if (rectangle)
        {
            const auto b = memory(rectangle, 16);
            for (unsigned i = 0; i < 4; ++i)
                rect[i] = word(b, i * 4);
        }
        if (c.r8.u32)
        {
            const auto b = memory(c.r8.u32, 8);
            origin       = { word(b, 0), word(b, 4) };
        }
        t.insert("destination", c.r3.u32);
        t.insert("destination_pitch", c.r4.u32);
        t.insert("width", c.r5.u32);
        t.insert("height", c.r6.u32);
        t.insert("raw_destination_format", c.r7.u32);
        t.insert("destination_origin", toml::array{ origin[0], origin[1] });
        t.insert("source_pitch", c.r10.u32);
        t.insert("raw_source_format", sourceFormat);
        t.insert("source_rectangle", toml::array{ rect[0], rect[1], rect[2], rect[3] });
        t.insert("raw_stack_64", word(stack, 16));
        t.insert("provenance", "sub_826B5F30 826B5F60..826B61BC; stack +54 format, +5C rectangle, +64 raw control; linear rows");
        if (rect[2] < rect[0] || rect[3] < rect[1])
            throw std::runtime_error("scene_writer_rectangle");
        const auto* dst = rex::graphics::FormatInfo::Get(c.r7.u32 & 63);
        const auto* src = rex::graphics::FormatInfo::Get(sourceFormat & 63);
        if (!dst->block_width || !dst->block_height || !src->block_width || !src->block_height)
            throw std::runtime_error("scene_writer_format");
        const auto width = rect[2] - rect[0], height = rect[3] - rect[1];
        if (width > 16384 || height > 16384)
            throw std::runtime_error("scene_writer_rectangle_extent");
        const auto rows     = (origin[1] % dst->block_height + height + dst->block_height - 1) / dst->block_height;
        const auto rowBytes = ((origin[0] % dst->block_width + width + dst->block_width - 1) / dst->block_width) * dst->bytes_per_block();
        const auto address  = std::uint64_t(c.r3.u32) + (origin[1] / dst->block_height) * std::uint64_t(c.r4.u32) +
                              (origin[0] / dst->block_width) * dst->bytes_per_block();
        if (address > UINT32_MAX)
            throw std::runtime_error("scene_writer_address");
        linearDestination(w, static_cast<std::uint32_t>(address), c.r4.u32, rows, rowBytes);
        const auto srcRows     = (rect[1] % src->block_height + height + src->block_height - 1) / src->block_height;
        const auto srcRowBytes = ((rect[0] % src->block_width + width + src->block_width - 1) / src->block_width) * src->bytes_per_block();
        source(t, std::uint64_t(c.r9.u32) + (rect[1] / src->block_height) * std::uint64_t(c.r10.u32) + (rect[0] / src->block_width) * src->bytes_per_block(), srcRows ? std::uint64_t(srcRows - 1) * c.r10.u32 + srcRowBytes : 0);
    }
    else if (e == 0x82521558)
    {
        const auto pixel = memory(c.r3.u32, 104), pair = memory(c.r5.u32, 8);
        const auto selected = memory(c.r6.u32, 40), base = memory(c.r7.u32, 40);
        t.insert("pixel_data_object", c.r3.u32);
        t.insert("mip", c.r4.u32);
        t.insert("pitch_address_pair", words(pair));
        t.insert("raw_selected_descriptor", words(selected));
        t.insert("raw_base_descriptor", words(base));
        t.insert("slice", c.r8.u32);
        t.insert("provenance", "sub_82521558 82521578..82521700; pixel +50 data, +54 pitches, +5C offsets, +60 level count, +64 pitch multiplier");
        if (c.r4.u32 >= word(pixel, 96) || word(pixel, 96) > 32)
            throw std::runtime_error("scene_writer_mip");
        const auto offsets = memory(word(pixel, 92), (word(pixel, 96) + 1ULL) * 4);
        const auto offset = word(offsets, c.r4.u32 * 4), next = word(offsets, (c.r4.u32 + 1) * 4);
        if (next < offset)
            throw std::runtime_error("scene_writer_offsets");
        t.insert("source_mip_offset", offset);
        t.insert("source_next_mip_offset", next);
        const auto address = std::uint64_t(word(pixel, 80)) + offset + std::uint64_t(word(offsets, word(pixel, 96) * 4)) * c.r8.u32;
        source(t, address, next - offset);
        // Tiled destinations are collected from the filtered copy calls. Linear
        // copies either span a whole mip or use base descriptor +8 rows.
        if (!(word(selected, 16) & 0x100))
        {
            const auto pitch    = word(memory(std::uint64_t(word(pixel, 84)) + c.r4.u32 * 4ULL, 4), 0);
            const auto rowBytes = std::uint64_t(pitch) * word(pixel, 100);
            if (rowBytes > UINT32_MAX)
                throw std::runtime_error("scene_writer_pitch");
            const auto format = word(pixel, 12);
            if (word(pair, 0) == rowBytes || (format >= 4 && format <= 6))
                destination(w, word(pair, 4), next - offset);
            else
                linearDestination(w, word(pair, 4), word(pair, 0), word(base, 8), static_cast<std::uint32_t>(rowBytes));
        }
    }
    else if (e == 0x82305648)
    {
        t.insert("owner", c.r3.u32);
        t.insert("backend", word(memory(std::uint64_t(c.r3.u32) + 40, 4), 0));
        t.insert("raw_owner_28", word(memory(std::uint64_t(c.r3.u32) + 28, 4), 0));
        t.insert("mip", c.r4.u32);
        t.insert("region_count", c.r5.u32);
        if (c.r5.u32 > 1024)
            throw std::runtime_error("scene_writer_region_capacity");
        toml::array regions;
        const auto  records = memory(c.r6.u32, c.r5.u32 * 24ULL);
        for (std::size_t offset = 0; offset < records.size(); offset += 24)
            regions.push_back(words(records.subspan(offset, 24)));
        t.insert("region_records", std::move(regions));
        const auto image = memory(c.r7.u32, 28);
        t.insert("decoded_image_words", words(image));
        source(t, word(image, 16), word(image, 20));
        t.insert("provenance", "sub_82305648 82305658..8230589C; owner +28 raw wrapper, +40 lock backend; 24-byte records dst XY, source LTRB; image +0 format, +12 pitch, +16 bytes, +20 size (RVA-F-0029)");
        t.insert("destination_evidence", "nested filtered copy ranges");
        if (word(image, 0) == 2 && c.r5.u32)
        {
            // The last lock pair remains in this function's own frame after
            // unlock and the register-restoring tail. Other direct regions stay
            // explicitly partial, rather than inventing their lock addresses.
            const auto last = records.last(24);
            if (c.r1.u32 < 144 || word(last, 16) < word(last, 8) || word(last, 20) < word(last, 12))
                throw std::runtime_error("scene_writer_region_extent");
            const auto width = word(last, 16) - word(last, 8);
            if (width > 16384)
                throw std::runtime_error("scene_writer_region_extent");
            w.directRegionPair     = c.r1.u32 - 224 + 80;
            w.directRegionRows     = word(last, 20) - word(last, 12);
            w.directRegionRowBytes = width * 4;
            t.insert_or_assign("destination_evidence", "sub_82305648 last region lock pair at entry SP-144; format-2 RGB to RGBA direct stores");
            t.insert("direct_region_coverage", "last region only; earlier direct regions remain open");
        }
    }
    else if (e == 0x82C69850)
    {
        t.insert("owner", c.r3.u32);
        const auto input = word(memory(std::uint64_t(c.r3.u32) + 36, 4), 0);
        const auto b     = memory(input, 20);
        t.insert("sample_record", input);
        t.insert("sample_width", word(b, 8));
        t.insert("sample_height", word(b, 4));
        t.insert("sample_x_stride", word(b, 12));
        t.insert("sample_y_stride", word(b, 16));
        t.insert("raw_scale_bits", word(memory(std::uint64_t(c.r3.u32) + 88, 4), 0));
        const auto size = word(b, 4) && word(b, 8) ? (word(b, 4) - 1ULL) * word(b, 16) + (word(b, 8) - 1ULL) * word(b, 12) + 2 : 0;
        source(t, word(b, 0), size);
        t.insert("provenance", "sub_82C69850 82C69860..82C69B4C; owner +36 sample record, +88 raw float scale; unsigned 16-bit samples; nested tiler destinations");
    }
}

struct TrackedResource
{
    std::uint64_t                generation = 0, version = 0;
    bool                         historyCandidate = false, gpu = false;
    std::vector<SceneGuestRange> ranges;
};

// What one session retained for a resource generation. Each window keeps its own: retained
// bindings name blobs in that window's payload store, so they never carry over to a successor.
struct ResourcePayloads
{
    std::uint64_t                                                            generation = 0;
    bool                                                                     sampled = false, preArmRegionOverflow = false;
    std::vector<ScenePayloadBinding>                                         history;
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::string>> sampledHashes;
    std::unordered_map<std::uint64_t, std::uint64_t>                         sampledAllocations;
};

// Guest resource identity that outlives a capture window. Sessions of successive windows share one
// instance, so a later window still knows the textures created before it opened.
struct Tracking
{
    std::mutex                                                             payloadGate;
    std::unordered_map<std::uint32_t, TrackedResource>                     resources;
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> epochs;
    std::uint64_t                                                          payloadVersion = 0, payloadGeneration = 0;
    SceneAllocationTracker                                                 allocations;
    SceneTextureStorage                                                    storage;
};

struct Session
{
    using Resource = TrackedResource;

    explicit Session(std::shared_ptr<Tracking> shared)
    : tracking(std::move(shared))
    , payloadGate(tracking->payloadGate)
    , resources(tracking->resources)
    , epochs(tracking->epochs)
    , payloadVersion(tracking->payloadVersion)
    , payloadGeneration(tracking->payloadGeneration)
    , allocations(tracking->allocations)
    , storage(tracking->storage)
    {
    }

    std::shared_ptr<Tracking>                                               tracking;
    std::unique_ptr<ScenePayloadStore>                                      payloads;
    std::unique_ptr<SceneTargetReadback>                                    targets;
    std::string                                                             targetError;
    bool                                                                    effects = false;
    std::mutex&                                                             payloadGate;
    std::unordered_map<std::uint32_t, Resource>&                            resources;
    std::unordered_map<std::uint32_t, ResourcePayloads>                     resourcePayloads; // Guarded by payloadGate.
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>>& epochs;
    std::uint64_t&                                                          payloadVersion;
    std::uint64_t&                                                          payloadGeneration;
    std::atomic<std::uint64_t>                                              payloadGaps{ 0 }, payloadFailures{ 0 }, payloadAllocatorGaps{ 0 };
    std::string                                                             payloadError;
    SceneAllocationTracker&                                                 allocations;
    std::atomic<std::uint64_t>                                              allocationRecords{ 0 }, allocationCalls{ 0 }, freeCalls{ 0 }, allocationOpening{ 0 };
    std::atomic<std::uint64_t>                                              allocationFailures{ 0 }, regionFailures{ 0 }, regionRecords{ 0 };
    std::uint64_t                                                           preArmRegionBudget = 65536;
    std::atomic<std::uint64_t>                                              preArmRegionRecords{ 0 }, preArmRegionOverflow{ 0 }, preArmRegionFailures{ 0 };
    std::filesystem::path                                                   directory;
    std::string                                                             title, sdk, error, firstReadFailure;
    SceneLog                                                                log;
    SceneTextureStorage&                                                    storage;
    std::mutex                                                              gate;
    std::uint64_t                                                           sequence = 0, frame = 0, begin = 0, end = 0, events = 0, draws = 0, inflight = 0, readFailures = 0, relevantReadFailures = 0, capacityOverflow = 0;
    std::uint64_t                                                           outputBytes = 0;
    std::uint32_t                                                           frames = 2, armFrame = 0;
    bool                                                                    arm = false, window = false, closing = false, written = false, aborted = false;
    // Window bookkeeping. root is the capture directory; window 1 writes there and window k >= 2
    // writes root/window-<k>. A window opens on its arm file or, for window 1, on a cvar trigger.
    std::filesystem::path                          root;
    std::string                                    armName     = "arm", trigger, pixelDigest;
    std::uint32_t                                  windowIndex = 1, windows = 1, armDelay = 0, payloadBudget = 0;
    std::uint64_t                                  digestDue = 0;
    std::jthread                                   worker;
    std::unordered_map<std::string, std::uint32_t> shaderMasks;
    std::unordered_set<std::string>                shaderMaskErrors;

    void Failure(std::string_view reason, bool relevant = true)
    {
        std::lock_guard lock(gate);
        ++readFailures;
        if (writerCapacityFailure(reason))
        {
            ++capacityOverflow;
            relevant = true;
        }
        if (firstReadFailure.empty())
        {
            firstReadFailure = reason;
            REXLOG_ERROR("Scene resource capture read failure: {} (relevant: {})", reason, relevant);
        }
        if (relevant)
            ++relevantReadFailures;
        if (relevant && error.empty())
            error = reason;
    }

    void Release()
    {
        std::lock_guard lock(gate);
        --inflight;
    }
};

// Header, metadata and microcode of the shader object at G+0x3194 (pixel) or G+0x3198 (vertex).
struct ShaderLocation
{
    std::span<const std::uint8_t> header, meta, code;
    std::uint64_t                 address = 0;
    std::uint32_t                 size = 0, variant = 0;
};

ShaderLocation locateShader(std::uint32_t object, bool pixel, bool noPixelShader)
{
    ShaderLocation l;
    l.header          = memory(object, pixel ? 68 : 912);
    const auto offset = pixel ? 40U : 872U;
    l.variant         = !pixel && noPixelShader && (word(l.header, 872) & 0x20) ? 1U : 0U;
    l.meta            = memory(std::uint64_t(object) + offset + word(l.header, offset + 24 + l.variant * 8), 32);
    const auto raw    = std::uint64_t(word(l.header, pixel ? 24 : 32)) + word(l.meta, 0);
    if (raw > UINT32_MAX)
        throw std::runtime_error("scene_shader_alias_overflow");
    l.address = (raw & (pixel ? 0x1FFFFFFEULL : 0x1FFFFFFFULL)) + (((raw >> 20) + 512) & 0x1000);
    l.size    = word(l.meta, 4);
    if (!l.size || l.size > kMaxCode || l.size % 4)
        throw std::runtime_error("scene_shader_code_extent");
    l.code = memory(l.address, l.size, true);
    return l;
}

// Never reclaimed: observers that saw the relaxed pointer remain safe after close.
Session*              session = nullptr;
std::vector<Session*> sessions;
bool                  stopped = false;
std::mutex            lifecycle;

// Draw watches keep the draw hooks observing without a session: sceneCaptureSession then points
// at this marker, which BeginSceneEntry answers with an empty token and the frame boundary ignores.
struct DrawWatch
{
    std::string       prefix;
    SceneDrawWatchHit hit;
    void*             context;
};

// A draw holds watchClock shared from before it reads its shader until its hits are delivered,
// and a boundary holds it exclusively while it counts and selects, so a hit always sees the frame
// its draw ran in. Order: watchClock, then a session gate, then watchGate.
constexpr std::size_t    kMaxDrawWatches = 4;
std::shared_mutex        watchClock;
std::mutex               watchGate;
std::vector<DrawWatch>   watches;
std::atomic<std::size_t> pendingWatches{ 0 };
char                     watchOnlyMarker;

// Called by whoever stops observing for `closed`; a successor that already took over is left alone.
void releaseObservation(const void* closed, bool coverage) noexcept
{
    std::lock_guard lock(watchGate);
    void*           expected = const_cast<void*>(closed);
    sceneCaptureSession.compare_exchange_strong(expected, watches.empty() ? nullptr : &watchOnlyMarker, std::memory_order_relaxed);
    if (coverage)
    {
        expected = const_cast<void*>(closed);
        sceneCoverageSession.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
    }
}

// Reads the pixel shader the draw is about to use and fires the watches it matches.
void observeDrawWatch(const PPCContext& ctx) noexcept
{
    if (!pendingWatches.load(std::memory_order_relaxed))
        return;
    std::shared_lock       clock(watchClock);
    std::vector<DrawWatch> fired;
    try
    {
        const auto object = word(memory(std::uint64_t(ctx.r3.u32) + 0x3194, 4), 0);
        if (!object)
            return;
        const auto      wordDigest = ScenePixelWordDigest(locateShader(object, true, false).code);
        std::lock_guard lock(watchGate);
        for (auto it = watches.begin(); it != watches.end();)
        {
            if (wordDigest.starts_with(it->prefix))
            {
                fired.push_back(std::move(*it));
                it = watches.erase(it);
            }
            else
                ++it;
        }
        pendingWatches.store(watches.size(), std::memory_order_relaxed);
        void* marker = &watchOnlyMarker;
        if (watches.empty())
            sceneCaptureSession.compare_exchange_strong(marker, nullptr, std::memory_order_relaxed);
    }
    catch (...)
    {
        // An unreadable draw state cannot match; the capture proper reports its own read failures.
        return;
    }
    for (const auto& watch : fired)
        watch.hit(watch.context);
}

// This session's payload state for the current generation of a tracked resource.
ResourcePayloads& payloadsOf(Session& s, std::uint32_t object, const TrackedResource& r)
{
    auto& p = s.resourcePayloads[object];
    if (p.generation != r.generation)
    {
        p            = {};
        p.generation = r.generation;
    }
    return p;
}

bool sampledHere(const Session& s, std::uint32_t object, const TrackedResource& r)
{
    const auto it = s.resourcePayloads.find(object);
    return it != s.resourcePayloads.end() && it->second.generation == r.generation && it->second.sampled;
}

// Optional retention must never turn an otherwise usable metadata capture fatal.
template <class F>
void payloadAttempt(Session& s, toml::table& t, F&& action)
{
    try
    {
        action();
    }
    catch (const std::exception& e)
    {
        t.insert_or_assign("payload_error", e.what());
        ++s.payloadFailures;
    }
}

void shader(toml::table& t, ScenePacket& packet, std::span<const std::uint8_t> state, bool pixel)
{
    const auto  object = word(state, pixel ? 0x3194 : 0x3198);
    const auto  prefix = pixel ? "ps" : "vs";
    toml::table s{ { "object", object } };
    if (object)
    {
        const auto l      = locateShader(object, pixel, !word(state, 0x3194));
        auto&      target = pixel ? packet.pixelCode : packet.vertexCode;
        target.assign(l.code.begin(), l.code.end());
        s.insert("header_hex", hex(l.header));
        s.insert("metadata_hex", hex(l.meta));
        s.insert("source_address", static_cast<std::int64_t>(l.address));
        s.insert("source_bytes", l.size);
        s.insert("source_sha256", digest(l.code));
        s.insert("variant", l.variant);
        s.insert("identity_source", "G+3194/3198; sub_826ADD88 826ADE04..826ADE2C and PS metadata (pre-Original source)");
    }
    t.insert(prefix, std::move(s));
}

toml::table arguments(const PPCContext& c)
{
    toml::array a;
    for (auto v : { c.r3.u64, c.r4.u64, c.r5.u64, c.r6.u64, c.r7.u64, c.r8.u64, c.r9.u64, c.r10.u64 })
        a.push_back(std::to_string(v));
    return toml::table{ { "raw_r3_r10_u64", a }, { "raw_r1", c.r1.u32 }, { "raw_f1_bits", std::to_string(c.f1.u64) } };
}

std::uint64_t payloadEpoch(Session& s, const std::string& resource, std::span<const std::uint8_t> descriptor)
{
    if (!s.epochs.contains(resource) && s.epochs.size() >= 65536)
        throw std::runtime_error("scene_payload_identity_capacity");
    auto&      epoch = s.epochs[resource];
    const auto hash  = digest(descriptor);
    if (epoch.first != hash)
        epoch = { hash, ++s.payloadGeneration };
    return epoch.second;
}

ScenePayloadBinding retain(Session& s, const ScenePayloadCall& call, std::string role, std::uint64_t address, std::uint64_t size, std::string resource, std::uint64_t generation, bool physical = false, bool publish = true, std::uint64_t version = 0, std::uint64_t allocation = UINT64_MAX)
{
    ScenePayloadBinding b;
    b.frame             = call.frame;
    b.event             = call.event;
    b.submission        = call.submission;
    b.parent            = call.parent;
    b.generation        = generation;
    b.version           = version ? version : call.event;
    b.address           = address;
    b.size              = size;
    b.resource          = std::move(resource);
    b.role              = std::move(role);
    b.physical          = physical;
    const bool lifetime = physical && (b.role == "vertices" || b.role == "indices" || b.resource.starts_with("texture:"));
    if (lifetime)
    {
        b.allocationGeneration = allocation == UINT64_MAX ? s.allocations.Find({ address, size }) : allocation;
        if (!b.allocationGeneration)
            ++s.payloadAllocatorGaps;
    }
    return s.payloads->Capture(std::move(b), [&]
                               {
                                   return memory(address, size, physical);
                               },
                               publish);
}

std::uint64_t shaderPhysical(std::uint64_t raw)
{
    if (raw > UINT32_MAX)
        throw std::runtime_error("scene_payload_alias_extent");
    return (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
}

// solid_draw_adapter.cpp read dependencies, plus the literal backing records
// consumed by sub_826ADB98 at 826ADBA4..826ADC34 (RVA-F-0161).
std::vector<std::uint8_t> shaderPayloads(Session& s, const ScenePayloadCall& call, std::span<const std::uint8_t> state)
{
    std::array<std::vector<std::uint8_t>, 2> headers, metadata;
    std::array<std::uint64_t, 2>             addresses{}, generations{};
    std::array<std::uint32_t, 2>             objects{};
    std::vector<std::uint8_t>                records;
    for (unsigned stage = 0; stage < 2; ++stage)
    {
        const bool ps     = stage == 1;
        const auto object = word(state, ps ? 0x3194 : 0x3198);
        objects[stage]    = object;
        if (!object)
            continue;
        const auto resource = "shader:" + std::to_string(object);
        const auto h        = memory(object, ps ? 68 : 912);
        headers[stage].assign(h.begin(), h.end());
        const auto generation = generations[stage] = payloadEpoch(s, resource, h);
        retain(s, call, ps ? "ps_header" : "vs_header", object, h.size(), resource, generation);
        const auto off     = ps ? 40U : 872U;
        const auto variant = !ps && !word(state, 0x3194) && (word(h, off) & 0x20) ? 1U : 0U;
        const auto address = addresses[stage] = std::uint64_t(object) + off + word(h, off + 24 + variant * 8);
        const auto m                          = memory(address, 32);
        metadata[stage].assign(m.begin(), m.end());
        const auto rawCode = std::uint64_t(word(h, ps ? 24 : 32)) + word(m, 0);
        retain(s, call, ps ? "ps_code" : "vs_code", shaderPhysical(ps ? rawCode & ~1ULL : rawCode), word(m, 4), resource, generation, true);
        const auto literalOffset = word(h, off + 20);
        if (literalOffset)
        {
            const auto table = std::uint64_t(object) + off + literalOffset;
            const auto size  = word(memory(table, 20), 16);
            // The 20-byte movie table is one record, a zero terminator and 4 pad bytes, so the size need not be a multiple of 8.
            if (size > kMaxLiteralTable || (size && size < 8))
                throw std::runtime_error("scene_literal_extent");
            // 0x826ADB98 reads whole 8-byte records while r31 < start + size, so the last one may end up to 7 bytes past it.
            const auto  literals = memory(table + 20, (size + 7ULL) & ~7ULL);
            std::size_t consumed = 0;
            for (std::size_t i = 0; i < size; i += 8)
            {
                const auto count = word(literals, i) & 0xFFFF;
                if (!count)
                    break;
                consumed = i + 8;
                if (count * 4ULL > kMaxCode)
                    throw std::runtime_error("scene_literal_values_extent");
            }
            retain(s, call, ps ? "ps_literal_table" : "vs_literal_table", table, 20 + std::max<std::size_t>(size, consumed), resource, generation);
            for (std::size_t i = 0; i < consumed; i += 8)
            {
                const auto backing = shaderPhysical(std::uint64_t(word(h, ps ? 24 : 32)) + word(literals, i + 4));
                retain(s, call, (ps ? "ps_literal_values:" : "vs_literal_values:") + std::to_string(word(literals, i) >> 16), backing, (word(literals, i) & 0xFFFF) * 4ULL, resource, generation, true);
            }
        }
    }
    const bool link = !metadata[0].empty() && !metadata[1].empty() &&
                      ((word(metadata[0], 20) ^ word(metadata[1], 20)) & 0x7FFFF) &&
                      (metadata[0][8] & 7) != 7 && !(word(metadata[0], 20) & 0x40000) && !(word(metadata[1], 20) & 0x20000);
    for (unsigned stage = 0; stage < 2; ++stage)
    {
        if (!objects[stage])
            continue;
        const auto    resource = "shader:" + std::to_string(objects[stage]);
        const auto&   m        = metadata[stage];
        const auto    count    = link ? (word(m, 20) >> 5) & 31 : 0;
        std::uint64_t extent   = 32 + count * 4;
        if (!stage)
        {
            const auto fetchCount = word(m, 28);
            const auto offset     = (std::uint64_t(word(m, 24)) + 9) * 4;
            if (fetchCount > 64 || offset + fetchCount * 4 + count * 4 > kMaxCode)
                throw std::runtime_error("scene_fetch_metadata_extent");
            const auto r = memory(addresses[0] + offset, fetchCount * 4);
            records.assign(r.begin(), r.end());
            retain(s, call, "fetch_records", addresses[0] + offset, r.size(), resource, generations[0]);
            retain(s, call, "fetch_templates", std::uint64_t(objects[0]) + 68, fetchCount * 12ULL, resource, generations[0]);
            const auto linkage = offset + fetchCount * 4, lists = linkage + count * 4;
            extent             = std::max<std::uint64_t>(32, lists);
            const auto indices = memory(addresses[0] + linkage, count * 4);
            for (unsigned i = 0; i < count; ++i)
            {
                const auto index      = (word(indices, i * 4) >> 16) & 0xFFF;
                bool       terminated = false;
                for (unsigned j = 0; j < 4096; ++j)
                {
                    const auto end = lists + (index + j + 1) * 4;
                    if (end > kMaxCode)
                        throw std::runtime_error("scene_linkage_extent");
                    extent = std::max(extent, end);
                    if (word(memory(addresses[0] + end - 4, 4), 0) & 0x1000)
                    {
                        terminated = true;
                        break;
                    }
                }
                if (!terminated)
                    throw std::runtime_error("scene_linkage_unterminated");
            }
        }
        retain(s, call, stage ? "ps_metadata" : "vs_metadata", addresses[stage], extent, resource, generations[stage]);
    }
    retain(s, call, "fetch_swizzle_table", 0x82091394, 16, "title_fetch_tables", 1);
    retain(s, call, "fetch_format_table", 0x820913A8, 64, "title_fetch_tables", 1);
    return records;
}

void geometryPayloads(Session& s, const ScenePayloadCall& call, const PPCContext& c, std::uint32_t entry, std::span<const std::uint8_t> state, std::span<const std::uint8_t> records, toml::table& t)
{
    const auto declaration = word(state, 0x2E2C);
    const auto h           = memory(declaration, 52);
    const auto count       = word(h, 24);
    if (!declaration || count > 64)
        throw std::runtime_error("scene_declaration_extent");
    const auto elements   = memory(std::uint64_t(declaration) + 52, count * 12);
    const auto key        = "declaration:" + std::to_string(declaration);
    const auto generation = payloadEpoch(s, key, memory(declaration, 52 + count * 12));
    retain(s, call, "declaration", declaration, 52 + count * 12, key, generation);
    if (entry == 0x826A3048 || entry == 0x826A2B58)
    {
        // Nested lowerers have no initialized UP bytes until their outer copy.
        if (!call.parent)
            throw std::runtime_error("scene_direct_lowerer_geometry_unavailable");
        return;
    }
    const bool bound = entry == 0x826A39F8, indexed = bound || entry == 0x826A3568;
    if (!indexed && entry != 0x826A3000)
        throw std::runtime_error("scene_automatic_geometry_unobserved");
    std::vector<std::uint32_t> vertices;
    if (indexed)
    {
        std::uint64_t address = c.r8.u32, indexGeneration = call.event;
        std::uint32_t width = (c.r9.u32 & 4) ? 4 : 2, endian = width == 4 ? 2 : 1;
        std::string   resource = "up_indices:" + std::to_string(address);
        if (bound)
        {
            const auto object = word(state, 0x3094);
            const auto ih     = memory(object, 32);
            resource          = "index:" + std::to_string(object);
            indexGeneration   = payloadEpoch(s, resource, ih);
            retain(s, call, "index_header", object, ih.size(), resource, indexGeneration);
            const auto flags = word(ih, 0);
            width            = flags & 0x80000000U ? 4 : 2;
            endian           = (flags >> 29) & 3;
            address          = shaderPhysical(std::uint64_t(word(ih, 24)) + std::uint64_t(c.r6.u32) * width);
            t.insert("index_header_flags", flags);
        }
        const auto n = c.r7.u32;
        if (n > 4 * 1024 * 1024)
            throw std::runtime_error("scene_index_count_extent");
        const auto begin = address & ~3ULL, end = (address + std::uint64_t(n) * width + 3) & ~3ULL;
        const auto ib = memory(begin, end - begin, bound);
        retain(s, call, "indices", begin, end - begin, resource, indexGeneration, bound);
        t.insert("index_address", static_cast<std::int64_t>(address));
        t.insert("index_width", width);
        t.insert("index_endian", endian);
        // G+28CC maps register 0x2100; PA_SU_SC_MODE_CNTL is 0x2205.
        const bool restart      = (word(state, 0x2948) & (1U << 21)) != 0;
        const auto restartIndex = word(state, 0x28D8) & 0xFFFFFFU;
        for (std::uint64_t i = 0; i < n; ++i)
        {
            std::uint32_t  value = 0;
            const unsigned swap  = endian == 1 ? 1 : endian == 2 ? 3
                                                 : endian == 3   ? 2
                                                                 : 0;
            for (unsigned j = 0; j < width; ++j)
                value |= std::uint32_t(ib[((address - begin + i * width + j) ^ swap)]) << (j * 8);
            if (restart && (value & 0xFFFFFFU) == restartIndex)
                continue;
            const auto vertex = std::int64_t(value) + (bound ? c.r5.s32 : 0);
            if (vertex < 0 || vertex > UINT32_MAX)
                throw std::runtime_error("scene_vertex_index_extent");
            if (!bound && (value < c.r5.u32 || std::uint64_t(value) >= std::uint64_t(c.r5.u32) + c.r6.u32))
                throw std::runtime_error("scene_up_vertex_extent");
            vertices.push_back(static_cast<std::uint32_t>(vertex));
        }
    }
    else
    {
        if (c.r5.u32 > 4 * 1024 * 1024)
            throw std::runtime_error("scene_vertex_count_extent");
        for (std::uint32_t i = 0; i < c.r5.u32; ++i)
            vertices.push_back(i);
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    for (std::size_t r = 0; r < records.size(); r += 4)
    {
        bool matched = false;
        for (unsigned element = 0; element < count; ++element)
            matched |= ((word(records, r) >> 12) & 15) == elements[element * 12 + 9] &&
                       ((word(records, r) >> 16) & 15) == elements[element * 12 + 10];
        if (!matched)
            throw std::runtime_error("scene_fetch_semantic_missing");
    }

    struct VertexRange
    {
        ScenePayloadRange span;
        std::string       resource;
        std::uint64_t     generation;
    };

    std::vector<VertexRange> referenced;
    toml::array              streams;
    for (unsigned element = 0; element < count; ++element)
    {
        const auto e    = elements.subspan(element * 12, 12);
        bool       used = false;
        for (std::size_t r = 0; r < records.size(); r += 4)
            used |= ((word(records, r) >> 12) & 15) == e[9] && ((word(records, r) >> 16) & 15) == e[10];
        if (!used)
            continue;
        const auto stream = word(e, 0) >> 16, offset = word(e, 0) & 0xFFFF;
        if (stream >= 16 || (!bound && stream))
            throw std::runtime_error("scene_vertex_stream_extent");
        const auto format      = word(e, 4) & 63;
        const auto formatBytes = memory(0x820913A8 + format, 1)[0] * 4U;
        if (!formatBytes || formatBytes > 16)
            throw std::runtime_error("scene_vertex_format_extent");
        std::uint64_t address = indexed ? c.r10.u32 : c.r6.u32, extent = kMaxRead;
        std::uint32_t stride   = indexed ? word(memory(std::uint64_t(c.r1.u32) + 0x54, 4), 0) : c.r7.u32;
        std::uint64_t epoch    = call.event;
        std::string   resource = "up_vertices:" + std::to_string(address);
        if (bound)
        {
            const auto descriptor = state.subspan(0x480 + (95 - stream) * 8, 8);
            if ((word(descriptor, 0) & 3) != 3)
                throw std::runtime_error("scene_vertex_fetch_type");
            address  = word(descriptor, 0) & ~3U;
            extent   = ((word(descriptor, 4) >> 2) & 0xFFFFFF) * 4ULL;
            stride   = state[0x30F0 + stream] * 4;
            resource = "vertex_fetch:" + std::to_string(address);
            epoch    = payloadEpoch(s, resource, descriptor);
        }
        if (!stride || offset + formatBytes > stride)
            throw std::runtime_error("scene_vertex_stride_extent");
        streams.push_back(toml::table{ { "stream", stream }, { "element", element }, { "address", static_cast<std::int64_t>(address) }, { "stride", stride }, { "generation", static_cast<std::int64_t>(epoch) }, { "generation_source", bound ? "observed fetch descriptor epoch; allocator lifetime unproved" : "UP call" } });
        // Retain one covering span per element: a stride wider than the used bytes would
        // otherwise produce a binding per vertex. Unused gap bytes inside it are retained too.
        std::vector<ScenePayloadRange> ranges;
        if (!vertices.empty())
        {
            const auto              firstByte = address + std::uint64_t(vertices.front()) * stride + offset;
            const auto              lastByte  = address + std::uint64_t(vertices.back()) * stride + offset + formatBytes;
            const ScenePayloadRange requested{ firstByte, lastByte - firstByte };
            const auto              clipped = ClipScenePayloadRange(requested, { address, extent });
            if (clipped.size != requested.size)
                throw std::runtime_error("scene_vertex_range_clipped");
            ranges.push_back(clipped);
        }
        for (const auto r : ranges)
            referenced.push_back({ r, resource, epoch });
    }
    std::sort(referenced.begin(), referenced.end(), [](const auto& a, const auto& b)
              {
                  return a.resource != b.resource ? a.resource < b.resource : a.span.address < b.span.address;
              });
    std::vector<VertexRange> merged;
    for (const auto& r : referenced)
    {
        if (!merged.empty() && r.resource == merged.back().resource && r.generation == merged.back().generation &&
            r.span.address <= merged.back().span.address + merged.back().span.size)
            merged.back().span.size = std::max(merged.back().span.address + merged.back().span.size, r.span.address + r.span.size) - merged.back().span.address;
        else
            merged.push_back(r);
    }
    if (merged.size() > kMaxWriterRanges)
        throw std::runtime_error("scene_geometry_range_capacity");
    for (const auto& r : merged)
        retain(s, call, "vertices", r.span.address, r.span.size, r.resource, r.generation, bound);
    if (bound)
    {
        t.insert("geometry_lifetime", "allocation_generation on each payload; zero denotes an unobserved lifetime");
    }
    t.insert("geometry_streams", std::move(streams));
}

void texturePayloads(Session& s, const ScenePayloadCall& call, const ScenePacket& p, toml::table& t)
{
    std::uint32_t bits = UINT32_MAX;
    try
    {
        bits = SceneTextureFetchMask(p.vertexCode) | SceneTextureFetchMask(p.pixelCode);
    }
    catch (const std::runtime_error& e)
    {
        t.insert_or_assign("payload_fetch_mask_error", e.what());
        t.insert("payload_fetch_mask_conservative", true);
        ++s.payloadGaps;
    }
    toml::array uses;
    for (unsigned slot = 0; slot < 32; ++slot)
    {
        if (!(bits & (1U << slot)))
            continue;
        const auto tex = texture(fetch(p.state, 0x480 + slot * 24));
        if (tex["type"].value_or(0U) != 2)
        {
            t.insert_or_assign("payload_gap", "shader texture demand has no texture binding");
            ++s.payloadGaps;
            uses.push_back(toml::table{ { "slot", slot }, { "gap", "unbound_texture" } });
            continue;
        }
        if (!tex.contains("base_bytes") || !tex.contains("mip_bytes"))
        {
            ++s.payloadGaps;
            uses.push_back(toml::table{ { "slot", slot }, { "gap", "unsupported_texture_layout" }, { "texture", tex } });
            continue;
        }
        for (const auto pair : { std::pair{ "base", "base_bytes" }, std::pair{ "mip_base", "mip_bytes" } })
        {
            const auto address = tex[pair.first].value_or(0U), size = tex[pair.second].value_or(0U);
            if (!address || !size)
                continue;
            Session::Resource* selected = nullptr;
            std::uint32_t      object   = 0;
            bool               gpu      = false;
            for (auto& [id, r] : s.resources)
                for (const auto range : r.ranges)
                {
                    const auto overlap = ClipScenePayloadRange({ address, size }, { range.address, range.size });
                    gpu |= overlap.size && r.gpu;
                    if (overlap.size == size && (!selected || r.generation > selected->generation))
                    {
                        selected = &r;
                        object   = id;
                    }
                }
            if (!selected)
            {
                ++s.payloadGaps;
                uses.push_back(toml::table{ { "slot", slot }, { "plane", pair.first }, { "gap", "scene_texture_allocation_unobserved" }, { "address", address }, { "bytes", size } });
                continue;
            }
            auto&      r        = *selected;
            auto&      retained = payloadsOf(s, object, r);
            const auto resource = "texture:" + std::to_string(object);
            if (!retained.sampled)
            {
                retained.sampled = true;
                for (auto history : retained.history)
                {
                    history.submission = call.submission;
                    s.payloads->Publish(std::move(history));
                }
                retained.history.clear();
            }
            const auto allocation = s.allocations.Find({ address, size });
            if (gpu && !allocation)
                ++s.payloadAllocatorGaps;
            if (!gpu)
            {
                auto&               previous           = retained.sampledHashes[(std::uint64_t(address) << 32) | size];
                auto&               previousAllocation = retained.sampledAllocations[(std::uint64_t(address) << 32) | size];
                ScenePayloadBinding payload;
                if (previous.first == r.version && previousAllocation == allocation && !previous.second.empty())
                {
                    payload                      = { call.frame, call.event, call.submission, call.parent, r.generation, r.version, address, size, resource, std::string("sampled_") + pair.first, previous.second, {}, true };
                    payload.allocationGeneration = allocation;
                    if (!payload.allocationGeneration)
                        ++s.payloadAllocatorGaps;
                }
                else
                {
                    payload = retain(s, call, std::string("sampled_") + pair.first, address, size, resource, r.generation, true, false, r.version, allocation);
                    if (payload.error.empty())
                    {
                        previous           = { r.version, payload.sha256 };
                        previousAllocation = allocation;
                    }
                }
                s.payloads->Publish(std::move(payload));
            }
            uses.push_back(toml::table{ { "slot", slot }, { "resource", resource }, { "generation", static_cast<std::int64_t>(r.generation) }, { "version", static_cast<std::int64_t>(r.version) }, { "plane", pair.first }, { "gpu_produced", gpu }, { "history_candidate", r.historyCandidate }, { "texture", tex }, { "allocation_generation", static_cast<std::int64_t>(allocation) } });
            uses.back().as_table()->insert("pre_arm_region_overflow", retained.preArmRegionOverflow);
        }
    }
    t.insert("payload_texture_uses", std::move(uses));
}

void drawPayloads(Session& s, ScenePayloadCall& call, const PPCContext& c, ScenePacket& p, toml::table& t)
{
    call.graphics = c.r3.u32;
    call.before   = p.state;
    retain(s, call, "state_pre", c.r3.u32, p.state.size(), "graphics:" + std::to_string(c.r3.u32), 1);
    payloadAttempt(s, t, [&]
                   {
                       const auto records = shaderPayloads(s, call, p.state);
                       geometryPayloads(s, call, c, p.entry, p.state, records, t);
                   });
    payloadAttempt(s, t, [&]
                   {
                       texturePayloads(s, call, p, t);
                   });
}

void payloadIdentity(Session& s, const ScenePayloadCall& call, toml::table& t)
{
    t.insert("capture_id", s.payloads->CaptureId());
    t.insert("call_id", static_cast<std::int64_t>(call.event));
    t.insert("parent_call_id", static_cast<std::int64_t>(call.parent));
    t.insert("submission_id", static_cast<std::int64_t>(call.submission));
}

void creationPayloads(Session& s, const ScenePayloadCall& call, toml::table& t, const std::vector<SceneGuestRange>& ranges)
{
    const auto object = t["object"].value_or(0U);
    if (!object || ranges.empty())
        return;
    auto&      r              = s.resources[object];
    const auto previous       = r.generation;
    const auto key            = "texture:" + std::to_string(object);
    const bool sameAllocation = previous && r.ranges.size() == ranges.size() &&
                                std::equal(r.ranges.begin(), r.ranges.end(), ranges.begin(), [](const auto& a, const auto& b)
                                           {
                                               return a.address == b.address && a.size == b.size;
                                           });
    if (!sameAllocation)
        r.generation = ++s.payloadGeneration;
    t.insert("allocation_generation", static_cast<std::int64_t>(r.generation));
    t.insert("previous_generation", static_cast<std::int64_t>(previous));
    if (r.generation == previous)
        return;
    r.version            = ++s.payloadVersion;
    r.ranges             = ranges;
    r.gpu                = false;
    auto&       retained = payloadsOf(s, object, r);
    const auto* tex      = t["texture"].as_table();
    r.historyCandidate   = tex && (((*tex)["format"].value_or(0U) == 2 && (*tex)["width"].value_or(0U) == 1024 && (*tex)["height"].value_or(0U) == 1024) ||
                                   ((*tex)["format"].value_or(0U) == 36 && (*tex)["width"].value_or(0U) == 512 && (*tex)["height"].value_or(0U) == 512));
    t.insert("pre_arm_history_candidate", r.historyCandidate);
    if (r.historyCandidate && s.payloads->Active())
        for (const auto range : ranges)
            retained.history.push_back(retain(s, call, "creation_initial_bytes", range.address, range.size, key, r.generation, true, false, r.version));
}

void writerSourcePayloads(Session& s, const ScenePayloadCall& call, SceneWriterState& w)
{
    const auto&       t = w.metadata;
    const std::string path{ t["writer_path"].value_or(std::string_view{}) };
    const auto        backend   = t["backend"].value_or(0U);
    bool              candidate = backend && s.resources.contains(backend) && (s.resources.at(backend).historyCandidate || sampledHere(s, backend, s.resources.at(backend)));
    // Lazy pre-arm C backends retain destination rows after lock establishes their identity.
    if (path == "C" && !call.inWindow && !candidate)
        return;
    // A C backend may be lazily created inside Original. These are the two
    // bounded owner shapes; an unmatched candidate is never published.
    if (path == "C")
    {
        const auto* image = t["decoded_image_words"].as_array();
        candidate |= image && image->size() == 7 && (*image)[1].value_or(0U) == 1024 && (*image)[2].value_or(0U) == 1024;
    }
    if (path == "D")
        candidate |= t["sample_width"].value_or(0U) == 512 && t["sample_height"].value_or(0U) == 512;
    if (!candidate)
        return;
    const auto address = t["source"].value_or<std::uint64_t>(0), size = t["source_bytes"].value_or<std::uint64_t>(0);
    const bool physical      = t["source_physical"].value_or(false);
    const auto sourceAddress = physical ? physicalAddress(static_cast<std::uint32_t>(address)) : address;
    if (size && path != "C")
        w.sourcePayloads.push_back(retain(s, call, "writer_source", sourceAddress, size, "pending_writer", 0, physical, false));
    if (path == "C")
    {
        // Region records preserve order, including the direct RGB->RGBA path.
        const auto* args = t["raw_r3_r10_u64"].as_array();
        if (args && args->size() == 8)
        {
            const auto records = std::stoull((*args)[3].value_or(std::string{}));
            w.sourcePayloads.push_back(retain(s, call, "writer_regions_24be", records, t["region_count"].value_or(0U) * 24ULL, "pending_writer", 0, false, false));
            const auto* image = t["decoded_image_words"].as_array();
            if (!image || image->size() != 7)
                throw std::runtime_error("scene_region_image_missing");
            const auto format = (*image)[0].value_or(0U), pitch = (*image)[3].value_or(0U);
            const auto pixelBytes = format == 2 ? 3U : format == 1 ? 4U
                                                   : format == 9   ? 1U
                                                                   : 0U;
            if (!pixelBytes)
                throw std::runtime_error("scene_region_source_format");
            const auto regions = memory(records, t["region_count"].value_or(0U) * 24ULL);
            for (std::size_t region = 0; region < regions.size(); region += 24)
            {
                const auto left = word(regions, region + 8), top = word(regions, region + 12);
                const auto right = word(regions, region + 16), bottom = word(regions, region + 20);
                if (right < left || bottom < top || bottom - top > 8192)
                    throw std::runtime_error("scene_region_source_extent");
                for (std::uint64_t row = top; row < bottom; ++row)
                {
                    if (w.sourcePayloads.size() >= kMaxWriterRanges)
                        throw std::runtime_error("scene_writer_payload_source_capacity");
                    const ScenePayloadRange requested{ sourceAddress + row * pitch + std::uint64_t(left) * pixelBytes, std::uint64_t(right - left) * pixelBytes };
                    if (ClipScenePayloadRange(requested, { sourceAddress, size }).size != requested.size)
                        throw std::runtime_error("scene_region_source_clipped");
                    w.sourcePayloads.push_back(retain(s, call, "writer_C_source:" + std::to_string(region / 24) + ":" + std::to_string(row - top), requested.address, requested.size, "pending_writer", 0, physical, false));
                }
            }
        }
    }
}

void writerPayloads(Session& s, const ScenePayloadCall& call, SceneWriterState& w, const std::vector<SceneGuestRange>& ranges, toml::table& t)
{
    toml::array updates;
    const auto  backend = t["backend_after"].value_or(t["backend"].value_or(0U));
    for (auto& [object, r] : s.resources)
    {
        const bool sampled = sampledHere(s, object, r);
        if (!r.historyCandidate && !sampled && !call.inWindow)
            continue;
        std::vector<ScenePayloadRange> clipped;
        for (const auto destination : ranges)
        {
            if (destination.address < 0xA0000000U)
                continue;
            for (const auto allocation : r.ranges)
            {
                auto span = ClipScenePayloadRange({ physicalAddress(destination.address), destination.size }, { allocation.address, allocation.size });
                if (span.size)
                    clipped.push_back(span);
            }
        }
        // C's direct conversion has no copy callback. Preserve the full resulting
        // image and its ordered source regions instead of inventing lock addresses.
        if (object == backend && w.metadata["writer_path"].value_or("") == std::string_view("C"))
        {
            clipped.clear();
            for (const auto range : r.ranges)
                clipped.push_back({ range.address, range.size });
        }
        if (clipped.empty())
            continue;
        if (r.gpu)
        {
            t.insert_or_assign("payload_gap", "CPU update overlaps GPU-owned storage; external seed/ownership join required");
            ++s.payloadGaps;
            continue;
        }
        r.version           = ++s.payloadVersion;
        const auto key      = "texture:" + std::to_string(object);
        auto&      retained = payloadsOf(s, object, r);
        for (auto source : w.sourcePayloads)
        {
            source.event      = call.event;
            source.frame      = call.frame;
            source.resource   = key;
            source.generation = r.generation;
            source.version    = r.version;
            if (sampled)
                s.payloads->Publish(std::move(source));
            else
                retained.history.push_back(std::move(source));
        }
        for (const auto span : clipped)
        {
            auto b = retain(s, call, "cpu_update", span.address, span.size, key, r.generation, true, sampled, r.version);
            if (!sampled)
                retained.history.push_back(std::move(b));
        }
        updates.push_back(toml::table{ { "resource", key }, { "generation", static_cast<std::int64_t>(r.generation) }, { "version", static_cast<std::int64_t>(r.version) }, { "order", static_cast<std::int64_t>(r.version) } });
        if (retained.history.size() > 262144)
            throw std::runtime_error("scene_payload_history_capacity");
    }
    if (t["writer_path"].value_or("") == std::string_view("C") && !updates.empty())
    {
        {
            std::lock_guard lock(s.gate);
            w.regionOutOfWindow |= call.inWindow ? !s.window : s.begin != 0;
        }
        t.insert("region_updates", w.regions);
        t.insert("pre_arm_regions", w.preArmRegions);
        if ((call.inWindow || w.preArmRegions) && !w.regionOutOfWindow && (w.regionPending || w.regions.size() != t["region_count"].value_or(0U)))
        {
            t.insert("payload_gap", "writer C region observations incomplete");
            ++(call.inWindow ? s.regionFailures : s.preArmRegionFailures);
        }
    }
    t.insert("payload_updates", std::move(updates));
}

void resolvePayloads(Session& s, const ScenePayloadCall& call, const PPCContext& c, toml::table& t)
{
    const auto sourceIndex = t["source_index"].value_or(5U);
    const auto b           = memory(c.r3.u32, 0x3500);
    if (call.inWindow)
        retain(s, call, "resolve_state_pre", c.r3.u32, b.size(), "graphics:" + std::to_string(c.r3.u32), 1);
    t.insert("completion_id", s.payloads->CaptureId() + ":resolve:" + std::to_string(call.event));
    t.insert("completion_state", "gpu_pending_external_readback");
    t.insert("surface_info_raw", word(b, 0x2880));
    t.insert("sample_count_log2", (word(b, 0x2880) >> 16) & 3);
    t.insert("clear_flags_raw", c.r4.u32);
    t.insert("clear_semantics", "raw flags and clear words retained; effective GPU copy/clear registers require readback join");
    t.insert("source_version_semantics", "conservative attachment write epoch; active target coverage requires effective draw state");
    if (sourceIndex <= 4)
    {
        const auto object     = word(b, 0x3098 + sourceIndex * 4);
        const auto key        = "attachment:" + std::to_string(object);
        const auto generation = object ? payloadEpoch(s, key, memory(object, 32)) : 0;
        t.insert("source_generation", static_cast<std::int64_t>(generation));
        t.insert("source_version", static_cast<std::int64_t>(s.resources[object].version));
        t.insert("source_resource", key);
    }
    const auto object = t["object"].value_or(0U);
    auto       found  = s.resources.find(object);
    if (found == s.resources.end())
    {
        t.insert("payload_gap", "scene_resolve_allocation_unobserved");
        ++s.payloadGaps;
        return;
    }
    auto& r = found->second;
    t.insert("destination_generation", static_cast<std::int64_t>(r.generation));
    t.insert("destination_version_before", static_cast<std::int64_t>(r.version));
    r.version = ++s.payloadVersion;
    r.gpu     = true;
    t.insert("destination_version_after", static_cast<std::int64_t>(r.version));
    toml::array aliases;
    for (auto& [otherObject, other] : s.resources)
        for (const auto a : r.ranges)
            for (const auto b : other.ranges)
                if (object != otherObject && ClipScenePayloadRange({ a.address, a.size }, { b.address, b.size }).size)
                {
                    aliases.push_back(toml::table{ { "object", otherObject }, { "generation", static_cast<std::int64_t>(other.generation) }, { "previous_version", static_cast<std::int64_t>(other.version) }, { "version", static_cast<std::int64_t>(r.version) } });
                    other.gpu     = true;
                    other.version = r.version;
                }
    t.insert("alias_transitions", std::move(aliases));
}

void postDrawPayloads(Session& s, const ScenePayloadCall& call, toml::table& t)
{
    if (call.before.empty() || !s.payloads->Active())
        return;
    const auto                after = memory(call.graphics, call.before.size());
    std::vector<std::uint8_t> delta;
    for (std::size_t offset = 0; offset < after.size(); offset += 4)
    {
        if (word(call.before, offset) == word(after, offset))
            continue;
        for (auto value : { static_cast<std::uint32_t>(offset), word(call.before, offset), word(after, offset) })
            for (unsigned byte = 0; byte < 4; ++byte)
                delta.push_back(static_cast<std::uint8_t>(value >> (24 - byte * 8)));
    }
    ScenePayloadBinding binding;
    binding.frame      = call.frame;
    binding.event      = call.event;
    binding.submission = call.submission;
    binding.parent     = call.parent;
    binding.resource   = "graphics:" + std::to_string(call.graphics);
    binding.generation = 1;
    binding.version    = call.event;
    binding.role       = "state_delta_offset_before_after_be32";
    binding.size       = delta.size();
    s.payloads->Capture(std::move(binding), [&]
                        {
                            return std::span<const std::uint8_t>(delta);
                        });
    t.insert("state_pre_sha256", digest(call.before));
    t.insert("state_post_sha256", digest(after));
    t.insert("state_changed_words", static_cast<std::int64_t>(delta.size() / 12));
    toml::array updates;
    for (unsigned slot = 0; slot < 5; ++slot)
        if (const auto object = word(after, 0x3098 + slot * 4))
        {
            auto& r   = s.resources[object];
            r.version = ++s.payloadVersion;
            if (r.generation)
                updates.push_back(toml::table{ { "resource", "texture:" + std::to_string(object) }, { "generation", static_cast<std::int64_t>(r.generation) }, { "version", static_cast<std::int64_t>(r.version) } });
        }
    t.insert("payload_updates", std::move(updates));
}

void decodeEvent(toml::table& t, ScenePacket& p, const PPCContext& c)
{
    const auto e = p.entry;
    // sub_82578E00 82578E80..82578ED0 rejects null target/source/rect
    // before dereferencing them. Retain the entry arguments without reading memory.
    if (e == 0x82578E00 && (!c.r3.u32 || !c.r6.u32 || !c.r10.u32))
    {
        t.insert("kind", "rejected_by_guest");
        t.insert("rejection_reason", "null upload target, source or rectangle; sub_82578E00 E_INVALIDARG");
        return;
    }
    t.insert("raw_stack_54_70", words(memory(std::uint64_t(c.r1.u32) + 0x54, 32)));
    if (drawEntry(e))
    {
        t.insert("kind", "draw");
        t.insert("graphics", c.r3.u32);
        const auto b = memory(c.r3.u32, 0x3500);
        shader(t, p, b, false);
        shader(t, p, b, true);
        toml::array bindings, attachments;
        for (unsigned i = 0; i < 32; ++i)
        {
            auto f = texture(fetch(b, 0x480 + i * 24));
            f.insert("slot", i);
            bindings.push_back(std::move(f));
        }
        for (unsigned i = 0; i < 5; ++i)
        {
            toml::table a{ { "source_index", i } };
            header(a, word(b, 0x3098 + i * 4), 32);
            attachments.push_back(std::move(a));
        }
        t.insert("bindings", std::move(bindings));
        t.insert("resolve_source_objects", std::move(attachments));
        t.insert("fetch_source", "SDK xenos.h xe_gpu_texture_fetch_t; sub_826ACEA0 G+480 groups of six BE words");
        t.insert("sampling_evidence", "static shader fetch demand; conditional execution is not observed");
        // A partially decoded draw must never enter worker binding/shader processing.
        p.state.assign(b.begin(), b.end());
    }
    else if (e == 0x8269E520)
    {
        t.insert("kind", "resolve");
        header(t, c.r6.u32);
        const auto w = fetch(memory(c.r6.u32, 0x34), 0x1C);
        const auto d = DecodeSceneResolve(c.r4.u32, w);
        t.insert("flags", c.r4.u32);
        t.insert("source_index", d.sourceIndex);
        if (d.sourceIndex <= 4)
        {
            toml::table a;
            header(a, word(memory(std::uint64_t(c.r3.u32) + 0x3098 + d.sourceIndex * 4, 4), 0), 32);
            t.insert("source_attachment", std::move(a));
        }
        t.insert("destination_base", d.primaryBase);
        t.insert("destination_format", d.format);
        t.insert("resolve_width", d.width);
        t.insert("resolve_height", d.height);
        t.insert("raw_layout_increment", d.layoutIncrement);
        t.insert("raw_resolve_pitch", d.rawPitch);
        t.insert("raw_resolve_endian", d.rawEndian);
        t.insert("mip", c.r8.u32);
        t.insert("slice", c.r9.u32);
        t.insert("raw_clear_r10", c.r10.u32);
        if (c.r5.u32)
            t.insert("rectangle", words(memory(c.r5.u32, 16)));
        else
            t.insert("rectangle", toml::array{ 0, 0, d.width, d.height });
        if (c.r7.u32)
            t.insert("destination_offset_xy", words(memory(c.r7.u32, 8)));
        else
            t.insert("destination_offset_xy", words(memory(0x820D783C, 8)));
        t.insert("raw_clear_words", words(memory(c.r10.u32 ? c.r10.u32 : 0x8208EBE0, 16)));
        t.insert("default_argument_source", "sub_8269E5B4..8269E5C8 and 8269E6AC..8269E6D8; selectors may change derived extents");
        t.insert("provenance", "sub_8269E520 8269E52C..8269E748; RVA-F-0118/0126; GPU output not read from RAM");
    }
    else if (e == 0x82304CF0)
    {
        t.insert("kind", "upload_input");
        const auto b = memory(c.r4.u32, 28);
        t.insert("decoded_image_words", words(b));
        t.insert("source", word(b, 16));
        t.insert("source_bytes", word(b, 20));
        t.insert("upload_format", word(b, 0));
        t.insert("width", word(b, 4));
        t.insert("height", word(b, 8));
        t.insert("pitch", word(b, 12));
        t.insert("levels", word(b, 24));
        t.insert("width_override", c.r5.u32);
        t.insert("height_override", c.r6.u32);
        t.insert("source_sha256", digest(memory(word(b, 16), word(b, 20))));
        t.insert("provenance", "RVA-F-0029 decodedImageRecord; original decoded input, before conversion");
    }
    else if (e == 0x82578E00)
    {
        t.insert("kind", "upload");
        const auto target = memory(c.r3.u32, 48);
        // sub_8269A918 at 8269A980..8269A9C4: exact 48-byte level wrapper,
        // +24 parent texture, +28 high nibble is the selected level.
        if ((word(target, 0) & ~0x200000U) == 0x40100004U)
        {
            t.insert("level_object", c.r3.u32);
            t.insert("level_header_hex", hex(target));
            t.insert("mip", word(target, 28) >> 28);
            header(t, word(target, 24));
            t.insert("target_source", "sub_8269A918 8269A980..8269A9C4; GFx call 823051E4/823052F8");
        }
        else
        {
            header(t, c.r3.u32);
            t.insert("raw_target", true);
            t.insert("target_source", "raw direct target descriptor interpretation; kind unproved");
        }
        const auto rect = memory(c.r10.u32, 16);
        t.insert("rectangle", words(rect));
        t.insert("source", c.r6.u32);
        t.insert("upload_format", c.r7.u32);
        t.insert("pitch", c.r8.u32);
        // The selected source span includes row padding. Retain its extent convention.
        const auto* f = rex::graphics::FormatInfo::Get(static_cast<xenos::TextureFormat>(c.r7.u32 & 63));
        if (!f->block_height)
            throw std::runtime_error("scene_upload_format_extent");
        const auto rows = (std::uint64_t(word(rect, 12)) + f->block_height - 1) / f->block_height;
        const auto size = rows * c.r8.u32;
        t.insert("source_bytes", static_cast<std::int64_t>(size));
        t.insert("source_sha256", digest(memory(c.r6.u32, size)));
        t.insert("hash_extent", "selected source through ceil(rect.bottom / format.block_height) * pitch; includes padding");
        t.insert("provenance", "RVA-F-0029 preUpload; sub_82578E00 82578F7C extracts r7 low-six-bit format");
    }
    else if (e == 0x8268FB08 || e == 0x8268FCB0)
    {
        t.insert("kind", e == 0x8268FB08 ? "target_bind" : "depth_bind_candidate");
        t.insert("graphics", c.r3.u32);
        t.insert("raw_slot", c.r4.u32);
        header(t, e == 0x8268FB08 ? c.r5.u32 : c.r4.u32, 32);
        t.insert("raw_offset", c.r6.u32);
        t.insert("raw_control_r7", c.r7.u32);
    }
    else if (e == 0x82518678 || e == 0x82697AA8)
    {
        t.insert("kind", "release");
        header(t, c.r3.u32, e == 0x82518678 ? 156 : 52);
        if (e == 0x82518678)
        {
            toml::array objects;
            for (auto off : { 0x90, 0x94 })
            {
                toml::table a;
                header(a, word(memory(std::uint64_t(c.r3.u32) + off, 4), 0));
                objects.push_back(std::move(a));
            }
            t.insert("released_slots", std::move(objects));
        }
    }
    else if (e == 0x826A4638)
    {
        t.insert("kind", "present");
        header(t, c.r4.u32);
    }
    else if (e == 0x8250AC90)
        t.insert("kind", "clear_resolve_enumeration");
    else
    {
        t.insert("kind", "create_begin");
        if (e == 0x826B7140)
        {
            header(t, c.r3.u32);
            t.insert("primary_base_add", c.r4.u32);
            t.insert("secondary_base_add", c.r5.u32);
        }
        if (e == 0x82533DB8)
        {
            t.insert("raw_r4", c.r4.u32);
            t.insert("raw_r5", c.r5.u32);
            t.insert("raw_r6", c.r6.u32);
            t.insert("raw_format_usage_r7", c.r7.u32);
            t.insert("raw_control_r8", c.r8.u32);
        }
        if (e == 0x8269AB90)
        {
            t.insert("raw_r3", c.r3.u32);
            t.insert("raw_r4", c.r4.u32);
            t.insert("raw_r5", c.r5.u32);
            t.insert("raw_r6", c.r6.u32);
            t.insert("raw_r7", c.r7.u32);
        }
    }
}

void push(Session& s, std::unique_ptr<ScenePacket> p, toml::table& t)
{
    t.insert_or_assign("sequence", static_cast<std::int64_t>(p->sequence));
    t.insert_or_assign("frame", static_cast<std::int64_t>(p->frame));
    t.insert_or_assign("entry", p->entry);
    std::ostringstream stream;
    stream << t;
    p->metadata = stream.str();
    s.log.Push(std::move(p));
}

void accountOutput(Session& s, std::uint64_t size)
{
    if (size > kMaxOutputBytes - s.outputBytes)
        throw std::runtime_error("scene_output_byte_capacity");
    s.outputBytes += size;
}

void binary(Session& s, const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    accountOutput(s, bytes.size());
    std::ofstream f(path, std::ios::binary | std::ios::noreplace);
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    f.close();
    if (!f)
        throw std::runtime_error("scene_binary_write");
}

std::uint32_t mask(Session& s, toml::table& t, const char* name, std::vector<std::uint8_t>& code, bool pixel)
{
    if (code.empty())
        return 0;
    auto&      metadata = *t[name].as_table();
    const auto hash     = metadata["source_sha256"].value<std::string>().value();
    const auto key      = std::string(pixel ? "ps-" : "vs-") + hash;
    if (auto found = s.shaderMasks.find(key); found != s.shaderMasks.end())
    {
        if (s.shaderMaskErrors.contains(key))
            metadata.insert("fetch_mask_conservative", true);
        return found->second;
    }
    binary(s, s.directory / (key + ".be.bin"), code);
    // A malformed exec clause demands every slot conservatively, so its draws
    // surface as open items instead of failing the whole capture.
    std::uint32_t bits = 0xFFFFFFFFU;
    try
    {
        bits = SceneTextureFetchMask(code);
    }
    catch (const std::runtime_error& error)
    {
        metadata.insert("fetch_mask_error", error.what());
        metadata.insert("fetch_mask_conservative", true);
        s.shaderMaskErrors.insert(key);
    }
    if (s.shaderMasks.size() >= 4096)
        throw std::runtime_error("scene_shader_identity_capacity");
    s.shaderMasks.emplace(key, bits);
    return bits;
}

void writeStatus(Session& s)
{
    auto status = [&]
    {
        std::lock_guard lock(s.gate);
        return toml::table{ { "ready", true }, { "fatal", !s.error.empty() }, { "first_read_failure", s.firstReadFailure }, { "failure_reason", s.error }, { "read_failures", static_cast<std::int64_t>(s.readFailures) }, { "relevant_read_failures", static_cast<std::int64_t>(s.relevantReadFailures) }, { "ignored_read_failures", static_cast<std::int64_t>(s.readFailures - s.relevantReadFailures) } };
    }();
    if (s.payloads)
    {
        status.insert("capture_id", s.payloads->CaptureId());
        status.insert("payload_bytes", static_cast<std::int64_t>(s.payloads->Bytes()));
        status.insert("payload_overflow", static_cast<std::int64_t>(s.payloads->Overflow()));
        status.insert("payload_read_failures", static_cast<std::int64_t>(s.payloads->Failures()));
        status.insert("payload_known_gaps", static_cast<std::int64_t>(s.payloadGaps.load()));
        status.insert("payload_failures", static_cast<std::int64_t>(s.payloadFailures.load()));
        status.insert("payload_allocator_gaps", static_cast<std::int64_t>(s.payloadAllocatorGaps.load()));
        status.insert("payload_failure_reason", s.payloadError);
        status.insert("payload_retention_stopped", !s.payloads->Active());
    }
    // Pollers never see a torn file. A reader holding status.toml open can block the rename on
    // Windows; status is best-effort progress, so drop that update and let the next one retry.
    const auto    pending = s.directory / "status.pending";
    std::ofstream f(pending, std::ios::trunc);
    f << status;
    f.close();
    if (!f)
        throw std::runtime_error("scene_status_write");
    std::error_code renamed;
    std::filesystem::rename(pending, s.directory / "status.toml", renamed);
    if (renamed)
        std::filesystem::remove(pending, renamed);
}

void writeResult(Session& s, bool stopped)
{
    if (s.targets)
    {
        try
        {
            s.targets->Flush(s.directory);
        }
        catch (const std::exception& e)
        {
            s.targetError = e.what();
        }
    }
    RouteEffectStats effectStats;
    std::string      effectError;
    if (s.effects)
    {
        FinishRouteEffects(effectError, &effectStats);
    }
    std::lock_guard lock(s.gate);
    s.aborted = stopped;
    s.closing = true;
    s.window  = false;
    if (stopped && s.error.empty())
        s.error = "stopped before window completion; inflight observers not awaited";
    const bool complete = !stopped && s.error.empty() && !s.log.Overflow() && !s.capacityOverflow && s.end == s.begin + s.frames && s.draws;
    if (!complete && s.error.empty())
        s.error = s.log.Overflow() ? "event overflow" : "empty or incomplete draw window";
    toml::table result{ { "schema", 1 }, { "complete", complete }, { "failure_reason", s.error }, { "begin_frame", static_cast<std::int64_t>(s.begin) }, { "end_frame", static_cast<std::int64_t>(s.end) }, { "requested_frames", s.frames }, { "arm_frame", s.armFrame }, { "events", static_cast<std::int64_t>(s.events) }, { "draws", static_cast<std::int64_t>(s.draws) }, { "overflow", static_cast<std::int64_t>(s.log.Overflow() + s.capacityOverflow) }, { "read_failures", static_cast<std::int64_t>(s.readFailures) }, { "relevant_read_failures", static_cast<std::int64_t>(s.relevantReadFailures) }, { "ignored_read_failures", static_cast<std::int64_t>(s.readFailures - s.relevantReadFailures) }, { "inflight_at_result", static_cast<std::int64_t>(s.inflight) }, { "boundary", "before VdSwap, half-open begin_frame..end_frame" }, { "queue_capacity", 16384 }, { "queue_byte_limit", 512 * 1024 * 1024 }, { "max_events", static_cast<std::int64_t>(kMaxEvents) }, { "max_draws", static_cast<std::int64_t>(kMaxDraws) }, { "output_bytes", static_cast<std::int64_t>(s.outputBytes) }, { "max_output_bytes", static_cast<std::int64_t>(kMaxOutputBytes) } };
    result.insert("window", s.windowIndex);
    result.insert("trigger", s.trigger);
    if (!s.pixelDigest.empty())
    {
        result.insert("arm_pixel_digest", s.pixelDigest);
        result.insert("arm_delay", s.armDelay);
    }
    if (s.effects)
    {
        // Scene completion is not an effects-completeness claim; read the effect header counters.
        result.insert("effect_file", "route-effects.bin");
        result.insert("effect_byte_budget", static_cast<std::int64_t>(kRouteEffectBudget));
        result.insert("effect_coverage", "entry_exit_samples_and_cursor_intervals");
        result.insert("effect_bytes", static_cast<std::int64_t>(effectStats.bytes));
        result.insert("effect_lost_records", static_cast<std::int64_t>(effectStats.lost));
        result.insert("effect_failed_reads", static_cast<std::int64_t>(effectStats.failedReads));
        result.insert("effect_gaps", static_cast<std::int64_t>(effectStats.gaps));
        result.insert("effect_unfinished_calls", static_cast<std::int64_t>(effectStats.unfinished));
        result.insert("effect_unchanged_fields", static_cast<std::int64_t>(effectStats.unchangedFields));
        result.insert("effect_failure_reason", effectError);
        result.insert("effect_complete", complete && effectError.empty() && !effectStats.lost && !effectStats.failedReads && !effectStats.gaps && !effectStats.unfinished);
    }
    if (s.targets)
    {
        const auto stats = s.targets->Stats();
        result.insert("target_manifest", "target-manifest.toml");
        result.insert("target_byte_budget", static_cast<std::int64_t>(s.targets->ByteBudget()));
        result.insert("target_record_budget", s.targets->RecordBudget());
        result.insert("target_bytes", static_cast<std::int64_t>(stats.bytes));
        result.insert("target_records", static_cast<std::int64_t>(stats.records));
        result.insert("target_failures", static_cast<std::int64_t>(stats.failures));
        result.insert("target_gaps", static_cast<std::int64_t>(stats.gaps));
        result.insert("target_overflow", static_cast<std::int64_t>(stats.overflow));
        result.insert("target_failure_reason", s.targetError);
        result.insert("target_complete", false);
        result.insert("target_provider", "unavailable: requires ordered opening-footprint and closing-present SDK readback");
    }
    if (s.payloads)
    {
        result.insert("capture_id", s.payloads->CaptureId());
        result.insert("payload_manifest", "payload-manifest.toml");
        result.insert("payload_byte_budget", static_cast<std::int64_t>(s.payloads->Budget()));
        result.insert("payload_bytes", static_cast<std::int64_t>(s.payloads->Bytes()));
        result.insert("payload_overflow", static_cast<std::int64_t>(s.payloads->Overflow()));
        result.insert("payload_read_failures", static_cast<std::int64_t>(s.payloads->Failures()));
        result.insert("payload_known_gaps", static_cast<std::int64_t>(s.payloadGaps.load()));
        result.insert("payload_failures", static_cast<std::int64_t>(s.payloadFailures.load()));
        result.insert("payload_allocator_gaps", static_cast<std::int64_t>(s.payloadAllocatorGaps.load()));
        result.insert("allocation_record_budget", 131072);
        result.insert("allocation_live_budget", 65536);
        result.insert("allocation_records", static_cast<std::int64_t>(s.allocationRecords));
        result.insert("allocation_calls", static_cast<std::int64_t>(s.allocationCalls));
        result.insert("allocation_free_calls", static_cast<std::int64_t>(s.freeCalls));
        result.insert("allocation_opening_records", static_cast<std::int64_t>(s.allocationOpening.load()));
        result.insert("allocation_failures", static_cast<std::int64_t>(s.allocationFailures.load()));
        result.insert("allocation_overflow", static_cast<std::int64_t>(s.allocations.Overflow()));
        result.insert("allocation_overlaps", static_cast<std::int64_t>(s.allocations.Overlaps()));
        result.insert("allocation_routes", toml::array{ "8280DAC0:r3 bytes,r4 flags,return r3", "8280DB58:r3 address,r4 flags" });
        result.insert("region_record_budget", 65536);
        result.insert("region_row_budget_per_writer", static_cast<std::int64_t>(kMaxWriterRanges));
        result.insert("region_records", static_cast<std::int64_t>(s.regionRecords.load()));
        result.insert("region_failures", static_cast<std::int64_t>(s.regionFailures.load()));
        result.insert("pre_arm_region_record_budget", static_cast<std::int64_t>(s.preArmRegionBudget));
        result.insert("pre_arm_region_records", static_cast<std::int64_t>(s.preArmRegionRecords.load()));
        result.insert("pre_arm_region_overflow", static_cast<std::int64_t>(s.preArmRegionOverflow.load()));
        result.insert("pre_arm_region_failures", static_cast<std::int64_t>(s.preArmRegionFailures.load()));
        result.insert("payload_failure_reason", s.payloadError);
        result.insert("payload_complete", complete && !s.payloads->Overflow() && !s.payloads->Failures() && !s.payloadGaps.load() && !s.payloadFailures.load());
        result.insert("gpu_completion", "external readback receipt required");
    }
    std::ofstream f(s.directory / "result.pending", std::ios::noreplace);
    f << result;
    f.close();
    if (!f)
        throw std::runtime_error("scene_result_write");
    std::filesystem::rename(s.directory / "result.pending", s.directory / "result.toml");
    s.written = true;
    REXLOG_INFO("Scene resource capture completed: {}", complete);
}

void flushPayloads(Session& s)
{
    if (!s.payloads || !s.payloadError.empty())
        return;
    try
    {
        s.payloads->Flush(s.directory);
    }
    catch (const std::exception& e)
    {
        s.payloadError = e.what();
        ++s.payloadFailures;
    }
}

void run(Session& s, std::stop_token stop)
{
    try
    {
        std::ofstream      log(s.directory / "events.toml", std::ios::binary | std::ios::noreplace);
        std::ostringstream preambleStream;
        preambleStream << "schema = 1\ntitle_revision = " << std::quoted(s.title) << "\nsdk_revision = " << std::quoted(s.sdk) << "\n";
        const auto preamble = preambleStream.str();
        accountOutput(s, preamble.size());
        log << preamble;
        flushPayloads(s);
        writeStatus(s);
        std::uint64_t statusFailures    = 0;
        bool          statusFatal       = false;
        bool          drained           = false;
        auto          payloadStatusTime = std::chrono::steady_clock::now();
        for (;;)
        {
            // A creation Original may be parked forever. Storage is retained, so
            // shutdown can publish failure without waiting for those observers.
            if (stop.stop_requested())
            {
                flushPayloads(s);
                log.close();
                writeResult(s, true);
                return;
            }
            bool queueEmpty = false;
            for (unsigned batch = 0; batch < 64; ++batch)
            {
                auto p = s.log.Pop();
                if (!p)
                {
                    queueEmpty = true;
                    break;
                }
                auto t = toml::parse(p->metadata);
                if (!p->state.empty())
                {
                    auto* bindings = t["bindings"].as_array();
                    if (!bindings)
                        throw std::runtime_error("scene_draw_bindings_missing");
                    const auto name = "draw-" + std::to_string(p->sequence) + ".state.be.bin";
                    binary(s, s.directory / name, p->state);
                    t.insert("state_file", name);
                    const auto bits = mask(s, t, "vs", p->vertexCode, false) | mask(s, t, "ps", p->pixelCode, true);
                    t.insert("static_texture_mask", bits);
                    for (auto& b : *bindings)
                    {
                        auto&      f    = *b.as_table();
                        const auto slot = f["slot"].value<std::uint32_t>().value();
                        f.insert("sampled", bool(bits & (1U << slot)));
                    }
                }
                toml::array rows;
                rows.push_back(std::move(t));
                std::ostringstream serialized;
                serialized << '\n'
                           << toml::table{ { "events", std::move(rows) } } << '\n';
                const auto text = serialized.str();
                accountOutput(s, text.size());
                log << text;
            }
            log.flush();
            flushPayloads(s);
            if (!log)
                throw std::runtime_error("scene_event_log_write");
            bool done = false, updateStatus = false;
            // A transient filesystem error leaves the window unarmed until the next poll.
            std::error_code armError;
            const bool      armed = std::filesystem::is_regular_file(s.root / s.armName, armError);
            {
                std::lock_guard lock(s.gate);
                if (!s.arm && armed)
                {
                    s.arm     = true;
                    s.trigger = "file";
                }
                done = s.closing && !s.inflight && queueEmpty;
                // A capacity or write failure sets error without a read failure.
                updateStatus   = statusFailures != s.readFailures || statusFatal != !s.error.empty();
                statusFailures = s.readFailures;
                statusFatal    = !s.error.empty();
            }
            if (updateStatus || (s.payloads && (done || std::chrono::steady_clock::now() - payloadStatusTime >= std::chrono::seconds(1))))
            {
                writeStatus(s);
                payloadStatusTime = std::chrono::steady_clock::now();
            }
            // Re-drain after observing zero writers: a final publication may follow the first drain.
            if (done && drained)
            {
                log.close();
                writeResult(s, false);
                return;
            }
            drained = done;
            if (queueEmpty)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    catch (const std::exception& e)
    {
        releaseObservation(&s, false);
        s.Failure(e.what());
        if (s.effects)
        {
            std::string effectError;
            FinishRouteEffects(effectError);
        }
        try
        {
            std::lock_guard lock(s.gate);
            s.closing = true;
            s.aborted = true;
            s.written = true;
            std::ofstream f(s.directory / "result.toml", std::ios::noreplace);
            f << toml::table{ { "complete", false }, { "failure_reason", s.error }, { "overflow", static_cast<std::int64_t>(s.log.Overflow() + s.capacityOverflow) }, { "events", static_cast<std::int64_t>(s.events) }, { "draws", static_cast<std::int64_t>(s.draws) }, { "read_failures", static_cast<std::int64_t>(s.readFailures) } };
        }
        catch (...)
        {
        }
        try
        {
            writeStatus(s);
        }
        catch (...)
        {
        }
        REXLOG_ERROR("Scene resource capture failed: {}", e.what());
    }
}

// A watch hit only schedules the opening; the boundary that reaches the due frame opens the window.
void onDigestHit(void* context) noexcept
{
    auto&           s = *static_cast<Session*>(context);
    std::lock_guard lock(s.gate);
    if (!s.arm && !s.closing && !s.digestDue)
        s.digestDue = s.frame + 1 + s.armDelay;
}

// Opens the next window's session at the boundary that closed `previous`. Resource identity and
// allocation lifetimes are shared; payload bytes retained for the previous window do not carry
// over, so textures created before this window are read again when they are first sampled.
void startSuccessor(Session& previous) noexcept
{
    std::lock_guard lock(lifecycle);
    if (stopped)
        return;
    try
    {
        const auto window    = previous.windowIndex + 1;
        const auto directory = previous.root / ("window-" + std::to_string(window));
        if (!std::filesystem::create_directories(directory))
            throw std::runtime_error("the scene window directory already exists");
        auto* s          = new Session(previous.tracking);
        s->directory     = directory;
        s->root          = previous.root;
        s->windowIndex   = window;
        s->windows       = previous.windows;
        s->frames        = previous.frames;
        s->title         = previous.title;
        s->sdk           = previous.sdk;
        s->armName       = "arm-" + std::to_string(window);
        s->payloadBudget = previous.payloadBudget;
        {
            std::lock_guard gateLock(previous.gate);
            s->frame = previous.frame;
        }
        if (previous.payloads)
        {
            const auto identity = directory.generic_string() + previous.title + previous.sdk +
                                  std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            s->payloads         = std::make_unique<ScenePayloadStore>(true, previous.payloadBudget, rex::crypto::sha256(identity));
        }
        sessions.push_back(s);
        s->worker = std::jthread([s](std::stop_token stop)
                                 {
                                     run(*s, stop);
                                 });
        // The opening snapshot defines the allocations this window's payloads may name. Taking it
        // and publishing the session under one payload gate puts every later allocator call in
        // this window's log and the effect of every earlier one in the snapshot.
        std::lock_guard payloadLock(s->payloadGate);
        if (s->payloads)
        {
            auto        p    = std::make_unique<ScenePacket>();
            const auto  live = s->allocations.Live();
            toml::array rows;
            for (const auto& [range, generation] : live)
                rows.push_back(toml::table{ { "address", static_cast<std::int64_t>(range.address) }, { "bytes", static_cast<std::int64_t>(range.size) }, { "allocation_generation", static_cast<std::int64_t>(generation) } });
            toml::table t{ { "kind", "allocation_snapshot" }, { "allocations", std::move(rows) } };
            {
                std::lock_guard windowLock(s->gate);
                p->sequence = ++s->sequence;
                p->frame    = s->frame;
                ++s->events;
            }
            s->allocationOpening = live.size();
            push(*s, std::move(p), t);
        }
        sceneCaptureSession.store(s, std::memory_order_release);
        if (s->payloads)
            sceneCoverageSession.store(s, std::memory_order_release);
        REXLOG_INFO("Scene resource capture window {} waits for {}", window, (s->root / s->armName).string());
    }
    catch (const std::exception& e)
    {
        REXLOG_ERROR("Scene resource capture could not open the next window: {}", e.what());
        releaseObservation(&previous, true);
    }
}

} // namespace

std::string ScenePixelWordDigest(std::span<const std::uint8_t> guestMicrocode)
{
    std::vector<std::uint8_t> host(guestMicrocode.begin(), guestMicrocode.end());
    for (std::size_t n = 0; n + 4 <= host.size(); n += 4)
        std::reverse(host.begin() + n, host.begin() + n + 4);
    return digest(host);
}

bool NormalizeScenePixelDigest(std::string_view text, std::string& normalized)
{
    if (text.size() < 8 || text.size() > 64)
        return false;
    std::string lower(text);
    for (auto& c : lower)
    {
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    normalized = std::move(lower);
    return true;
}

bool AddSceneDrawWatch(std::string_view digestPrefix, SceneDrawWatchHit hit, void* context, std::string& error)
{
    std::string prefix;
    if (!hit || !NormalizeScenePixelDigest(digestPrefix, prefix))
    {
        error = "a draw watch needs 8 to 64 hex digits of a pixel shader word digest";
        return false;
    }
    std::lock_guard lock(watchGate);
    if (watches.size() >= kMaxDrawWatches)
    {
        error = "too many draw watches";
        return false;
    }
    watches.push_back({ std::move(prefix), hit, context });
    pendingWatches.store(watches.size(), std::memory_order_relaxed);
    void* none = nullptr;
    sceneCaptureSession.compare_exchange_strong(none, &watchOnlyMarker, std::memory_order_release);
    return true;
}

void RemoveSceneDrawWatches(void* context) noexcept
{
    std::lock_guard lock(watchGate);
    std::erase_if(watches, [&](const DrawWatch& w)
                  {
                      return w.context == context;
                  });
    pendingWatches.store(watches.size(), std::memory_order_relaxed);
    void* marker = &watchOnlyMarker;
    if (watches.empty())
        sceneCaptureSession.compare_exchange_strong(marker, nullptr, std::memory_order_relaxed);
}

void SceneTextureStorage::Rebuild()
{
    std::vector<SceneGuestRange> next;
    for (const auto& [object, ranges] : objects)
        next.insert(next.end(), ranges.begin(), ranges.end());
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b)
              {
                  return a.address < b.address;
              });
    std::vector<SceneGuestRange> merged;
    for (const auto& r : next)
    {
        if (!r.size || std::uint64_t(r.address) + r.size > 0x20000000ULL)
            continue;
        if (!merged.empty() && r.address <= std::uint64_t(merged.back().address) + merged.back().size)
            merged.back().size = static_cast<std::uint32_t>(std::max(std::uint64_t(merged.back().address) + merged.back().size,
                                                                     std::uint64_t(r.address) + r.size) -
                                                            merged.back().address);
        else
            merged.push_back(r);
    }
    std::lock_guard lock(snapshotGate);
    snapshot.swap(merged);
}

bool SceneTextureStorage::Set(std::uint32_t object, std::vector<SceneGuestRange> ranges)
{
    std::lock_guard lock(updates);
    if (ranges.empty())
    {
        if (objects.erase(object))
            Rebuild();
        return true;
    }
    if (ranges.size() > 2 || (!objects.contains(object) && objects.size() >= 8192))
        return false;
    if (const auto found = objects.find(object); found != objects.end() &&
                                                 std::equal(ranges.begin(), ranges.end(), found->second.begin(), found->second.end(), [](const auto& a, const auto& b)
                                                            {
                                                                return a.address == b.address && a.size == b.size;
                                                            }))
        return true;
    objects.insert_or_assign(object, std::move(ranges));
    Rebuild();
    return true;
}

void SceneTextureStorage::Release(std::uint32_t object)
{
    std::lock_guard lock(updates);
    if (objects.erase(object))
        Rebuild();
}

bool SceneTextureStorage::Overlaps(std::uint32_t address, std::uint32_t size) const
{
    if (!size || address < 0xA0000000U || std::uint64_t(address) + size > 0x100000000ULL)
        return false;
    const auto begin = physicalAddress(address), end = begin + size;
    if (end > 0x20000000ULL)
        return false;
    std::shared_lock lock(snapshotGate);
    const auto       found = std::lower_bound(snapshot.begin(), snapshot.end(), end, [](const auto& r, std::uint64_t limit)
                                              {
                                            return r.address < limit;
                                              });
    return found != snapshot.begin() && std::uint64_t(std::prev(found)->address) + std::prev(found)->size > begin;
}

#if defined(REREVVED_SCENE_CAPTURE_TEST)
void SetSceneTestMemoryReader(SceneTestMemoryReader reader) noexcept
{
    testMemoryReader = reader;
}

void SetSceneTestPreArmRegionBudget(void* opaque, std::uint64_t budget) noexcept
{
    auto&           s = *static_cast<Session*>(opaque);
    std::lock_guard lock(s.payloadGate);
    s.preArmRegionBudget = budget;
}

bool SceneTestWindowActive(void* opaque) noexcept
{
    auto&           s = *static_cast<Session*>(opaque);
    std::lock_guard lock(s.gate);
    return s.window;
}

bool SceneTestArmed(void* opaque) noexcept
{
    auto&           s = *static_cast<Session*>(opaque);
    std::lock_guard lock(s.gate);
    return s.arm;
}

std::size_t SceneTestWriterRangeCount(const SceneHookToken& token) noexcept
{
    return token.writer ? token.writer->destinations.size() : 0;
}
#endif

void ObserveSceneAllocation(void* opaque, bool freed, std::uint32_t address, std::uint32_t bytes, std::uint32_t flags) noexcept
{
    auto& origin = *static_cast<Session*>(opaque);
    if (!origin.payloads || !address || !(flags & 0x80000000U) || address < 0xA0000000U)
        return;
    // The window that logs this call. A closed window's late call belongs to the successor once
    // that has taken its opening snapshot; before then the snapshot will include its effect. The
    // shared lifetimes are updated either way, so a later window never sees a stale allocation.
    Session* s = nullptr;
    try
    {
        std::lock_guard lock(origin.payloadGate);
        for (auto* candidate : { &origin, static_cast<Session*>(sceneCoverageSession.load(std::memory_order_acquire)) })
        {
            if (!candidate || candidate->tracking != origin.tracking)
                continue;
            std::lock_guard windowLock(candidate->gate);
            if (!candidate->closing)
            {
                s = candidate;
                ++s->inflight;
                break;
            }
        }
        const auto physical = physicalAddress(address);
        if (!s)
        {
            if (freed)
                origin.allocations.Free(physical);
            else
                origin.allocations.Allocate({ physical, bytes });
            return;
        }
        if (s->allocationRecords >= 131072)
        {
            ++s->allocationFailures;
            s->allocations.Invalidate();
        }
        else
        {
            auto p = std::make_unique<ScenePacket>();
            {
                std::lock_guard windowLock(s->gate);
                if (s->events + s->inflight > kMaxEvents)
                {
                    ++s->allocationFailures;
                    s->allocations.Invalidate();
                    --s->inflight;
                    return;
                }
                p->sequence = ++s->sequence;
                p->frame    = s->frame;
                ++s->events;
            }
            const auto generation = freed ? s->allocations.Free(physical) : s->allocations.Allocate({ physical, bytes });
            ++s->allocationRecords;
            if (freed)
                ++s->freeCalls;
            else
                ++s->allocationCalls;
            p->entry = freed ? 0x8280DB58 : 0x8280DAC0;
            toml::table t{ { "kind", freed ? "allocation_free" : "allocation" }, { "address", static_cast<std::int64_t>(physical) }, { "bytes", bytes }, { "flags", flags }, { "allocation_generation", static_cast<std::int64_t>(generation) } };
            push(*s, std::move(p), t);
        }
    }
    catch (...)
    {
        ++(s ? s : &origin)->allocationFailures;
        std::lock_guard lock(origin.payloadGate);
        origin.allocations.Invalidate();
    }
    if (s)
        s->Release();
}

void ObserveSceneWriterRegion(void* opaque, bool completed, std::uint32_t stack, std::uint32_t record, std::uint32_t owner, std::uint32_t image, std::uint32_t conversion) noexcept
{
    auto& s = *static_cast<Session*>(opaque);
    if (writerStack.empty() || !writerStack.back() || payloadStack.empty())
        return;
    auto& w = *writerStack.back();
    if (w.metadata["writer_path"].value_or("") != std::string_view("C"))
        return;
    const bool preArm = !payloadStack.back()->inWindow;
    try
    {
        std::lock_guard lock(s.payloadGate);
        {
            std::lock_guard windowLock(s.gate);
            if ((preArm ? s.begin != 0 : !s.window) || s.closing)
            {
                w.regionOutOfWindow = true;
                w.regionPending     = false;
                return;
            }
        }
        // Known non-candidates need no guest reads. A lazy backend is available after lock.
        auto backend = w.metadata["backend"].value_or(0U);
        if (!preArm || !backend)
            backend = word(memory(std::uint64_t(owner) + 40, 4), 0);
        if (preArm && (!s.resources.contains(backend) || !s.resources.at(backend).historyCandidate))
            return;
        w.preArmRegions = preArm;
        if (!completed)
        {
            if (w.regionPending)
                throw std::runtime_error("scene_region_unpaired_lock");
            const auto pair = memory(std::uint64_t(stack) + 80, 8);
            w.regionRecord  = record;
            w.regionPitch   = word(pair, 0);
            w.regionAddress = word(pair, 4);
            w.regionPending = true;
            return;
        }
        if (!w.regionPending || w.regionRecord != record)
            throw std::runtime_error("scene_region_unpaired_unlock");
        w.regionPending = false;
        if (preArm && s.preArmRegionRecords >= s.preArmRegionBudget)
        {
            ++s.preArmRegionOverflow;
            payloadsOf(s, backend, s.resources.at(backend)).preArmRegionOverflow = true;
            return;
        }
        if (preArm)
            ++s.preArmRegionRecords;
        else if (s.regionRecords.fetch_add(1) >= 65536)
            throw std::runtime_error("scene_region_record_budget");
        const auto r = memory(record, 24);
        const auto x = word(r, 0), y = word(r, 4), left = word(r, 8), top = word(r, 12), right = word(r, 16), bottom = word(r, 20);
        const auto format     = word(memory(image, 4), 0);
        const bool fallback   = conversion && format == 2;
        const auto pixelBytes = format == 9 ? 1U : (format == 1 || fallback) ? 4U
                                                                             : 0U;
        if (!pixelBytes || right < left || bottom < top || right - left > 16384 || bottom - top > 8192 ||
            x > UINT32_MAX - (right - left) || y > UINT32_MAX - (bottom - top))
            throw std::runtime_error("scene_region_extent");
        const auto rowBytes = (right - left) * pixelBytes;
        if (rowBytes > w.regionPitch)
            throw std::runtime_error("scene_region_pitch");
        toml::array rows;
        const auto  index = w.regions.size();
        for (std::uint32_t row = 0; row < bottom - top && rowBytes; ++row)
        {
            const auto address = std::uint64_t(w.regionAddress) + std::uint64_t(row) * w.regionPitch;
            destination(w, address, rowBytes);
            if (w.sourcePayloads.size() >= kMaxWriterRanges)
                throw std::runtime_error("scene_region_row_budget");
            auto b = retain(s, *payloadStack.back(), "writer_C_region:" + std::to_string(index) + ":" + std::to_string(row), address, rowBytes, "pending_writer", 0, false, false);
            if (address >= 0xA0000000ULL && address <= UINT32_MAX)
                b.allocationGeneration = s.allocations.Find({ physicalAddress(static_cast<std::uint32_t>(address)), rowBytes });
            if (!b.allocationGeneration)
                ++s.payloadAllocatorGaps;
            rows.push_back(toml::table{ { "row", row }, { "bytes", rowBytes }, { "sha256", b.sha256 }, { "error", b.error } });
            w.sourcePayloads.push_back(std::move(b));
        }
        w.regions.push_back(toml::table{ { "index", static_cast<std::int64_t>(index) }, { "backend", backend }, { "source_rect", toml::array{ left, top, right, bottom } }, { "destination_rect", toml::array{ x, y, x + right - left, y + bottom - top } }, { "rows_written", rowBytes ? bottom - top : 0 }, { "row_bytes", rowBytes }, { "fallback", fallback }, { "lock_address", w.regionAddress }, { "lock_pitch", w.regionPitch }, { "rows", std::move(rows) } });
    }
    catch (...)
    {
        ++(preArm ? s.preArmRegionFailures : s.regionFailures);
    }
}

SceneHookToken BeginSceneEntry(void* opaque, std::uint32_t entry, const PPCContext& ctx) noexcept
{
    if (!SceneEntryObserved(entry))
        return {};
    if (drawEntry(entry))
        observeDrawWatch(ctx);
    if (opaque == &watchOnlyMarker)
        return {};
    auto&          s = *static_cast<Session*>(opaque);
    SceneHookToken token{};
    bool           relevant      = true;
    bool           windowAtEntry = false;
    bool           writerPushed = false, payloadPushed = false;
    try
    {
        // The tiler only forwards copies to its enclosing upload or D writer.
        if (entry == 0x826B7E00)
            return {};
        // No reads, allocation or event reservation for unrelated hot copies.
        if (entry == 0x827F8550 && !s.storage.Overlaps(ctx.r3.u32, ctx.r5.u32))
            return {};
        relevant = !writerEntry(entry);
        if (entry == 0x827F8550 && !writerStack.empty())
        {
            // A parent that already failed is open; later spans add nothing.
            if (auto* parent = writerStack.back(); parent && !parent->metadata.contains("read_error"))
            {
                try
                {
                    destination(*parent, ctx.r3.u32, ctx.r5.u32);
                }
                catch (const std::exception& e)
                {
                    if (parent->payloadOnly)
                    {
                        parent->metadata.insert_or_assign("payload_error", e.what());
                        ++s.payloadFailures;
                    }
                    else
                    {
                        parent->metadata.insert_or_assign("read_error", e.what());
                        parent->metadata.insert_or_assign("failure_relevant", writerCapacityFailure(e.what()));
                        s.Failure(e.what(), false);
                    }
                }
            }
            return {};
        }
        if (entry == 0x82697AA8 || entry == 0x82518678)
            s.storage.Release(ctx.r3.u32);
        auto p = std::make_unique<ScenePacket>();
        {
            std::lock_guard lock(s.gate);
            if (s.closing || (SceneEntryWindowOnly(entry) && !s.window))
                return {};
            // Writers publish once on return; creations retain entry/return pairs.
            const bool paired = createEntry(entry) || (s.payloads && s.window && (drawEntry(entry) || entry == 0x8269E520 || entry == 0x82578E00 || entry == 0x82304CF0));
            if (s.events + s.inflight + (paired ? 2 : 1) > kMaxEvents || s.inflight >= 64 || (drawEntry(entry) && s.draws >= kMaxDraws))
            {
                s.closing = true;
                s.window  = false;
                if (s.effects)
                    SetRouteEffectWindow(false, s.frame);
                s.error = "scene event/draw lifetime capacity";
                ++s.capacityOverflow;
                releaseObservation(&s, false);
                return {};
            }
            p->sequence   = writerEntry(entry) && !s.payloads ? 0 : ++s.sequence;
            p->frame      = s.frame;
            p->entry      = entry;
            windowAtEntry = s.window;
            relevant      = !writerEntry(entry) && (s.window || createEntry(entry) || entry == 0x82304CF0 || entry == 0x82578E00 || entry == 0x8269E520 || entry == 0x82518678 || entry == 0x82697AA8);
            if (!writerEntry(entry))
                ++s.events;
            ++s.inflight;
            if (drawEntry(entry))
                ++s.draws;
            token = { &s, entry, ctx.r3.u32, p->sequence, ctx.lr, {} };
        }
        auto t = arguments(ctx);
        t.insert("caller_lr", std::to_string(ctx.lr));
        if (s.payloads)
        {
            token.payload = std::make_shared<ScenePayloadCall>();
            auto& call    = *token.payload;
            call.frame    = p->frame;
            call.event    = p->sequence;
            call.inWindow = windowAtEntry;
            if (!payloadStack.empty())
            {
                call.parent     = payloadStack.back()->event;
                call.submission = payloadStack.back()->submission;
            }
            if (drawEntry(entry) && !call.submission)
                call.submission = call.event;
            payloadIdentity(s, call, t);
        }
        try
        {
            if (writerEntry(entry))
            {
                token.writer = std::make_shared<SceneWriterState>();
                decodeWriter(*token.writer, entry, ctx);
                if (s.payloads && entry == 0x82305648)
                {
                    token.writer->directRegionPair = 0;
                    token.writer->metadata.erase("direct_region_coverage");
                    token.writer->metadata.insert_or_assign("destination_evidence", "per-region lock and pre-unlock rows");
                }
                t = token.writer->metadata;
            }
            else
                decodeEvent(t, *p, ctx);
            if (s.payloads)
            {
                payloadAttempt(s, t, [&]
                               {
                                   std::lock_guard payloadLock(s.payloadGate);
                                   if (entry == 0x82697AA8 || entry == 0x82518678)
                                   {
                                       s.resources.erase(ctx.r3.u32);
                                       s.resourcePayloads.erase(ctx.r3.u32);
                                   }
                                   if (s.payloads->Active())
                                   {
                                       if (drawEntry(entry))
                                           drawPayloads(s, *token.payload, ctx, *p, t);
                                       else if (entry == 0x8269E520)
                                           resolvePayloads(s, *token.payload, ctx, t);
                                       else if (token.writer)
                                           writerSourcePayloads(s, *token.payload, *token.writer);
                                   }
                                   else
                                       t.insert("payload_retention_stopped", true);
                                   payloadIdentity(s, *token.payload, t);
                               });
                if (token.writer)
                    token.writer->metadata = t;
            }
            if (entry == 0x82697AA8 || entry == 0x82518678)
            {
                if (const auto* slots = t["released_slots"].as_array())
                    for (const auto& slot : *slots)
                    {
                        s.storage.Release((*slot.as_table())["object"].value_or(0U));
                        if (s.payloads)
                        {
                            std::lock_guard payloadLock(s.payloadGate);
                            const auto      object = (*slot.as_table())["object"].value_or(0U);
                            s.resources.erase(object);
                            s.resourcePayloads.erase(object);
                        }
                    }
            }
        }
        catch (const std::exception& e)
        {
            if (token.writer)
                t = token.writer->metadata;
            t.insert_or_assign("read_error", e.what());
            t.insert_or_assign("failure_relevant", relevant || writerCapacityFailure(e.what()));
            s.Failure(e.what(), relevant);
            if (token.payload && (relevant || token.payload->inWindow || (token.writer && !token.writer->sourcePayloads.empty())))
                ++s.payloadGaps;
            if (token.writer)
                token.writer->metadata = t;
        }
        token.suppressCopies = entry == 0x82578E00 || entry == 0x82304CF0;
        if (token.suppressCopies && s.payloads)
        {
            token.writer              = std::make_shared<SceneWriterState>();
            token.writer->metadata    = t;
            token.writer->payloadOnly = true;
        }
        if (!writerEntry(entry))
            push(s, std::move(p), t);
        if (!createEntry(entry) && !writerEntry(entry) && !token.suppressCopies && !(s.payloads && windowAtEntry && (drawEntry(entry) || entry == 0x8269E520)))
        {
            s.Release();
            return {};
        }
        if (token.writer)
        {
            token.writer->metadata.insert("entry_frame", static_cast<std::int64_t>(s.payloads ? token.payload->frame : p->frame));
            writerStack.push_back(token.writer.get());
            writerPushed = true;
        }
        else if (token.suppressCopies)
        {
            writerStack.push_back(nullptr);
            writerPushed = true;
        }
        if (token.payload)
        {
            payloadStack.push_back(token.payload.get());
            payloadPushed = true;
        }
        if (s.effects && drawEntry(entry) && token.payload)
        {
            const auto& call  = *token.payload;
            auto        begun = BeginRouteEffectDraw(entry, ctx, RouteEffectIdentity{ call.frame, call.event, call.parent, call.submission });
            try
            {
                token.effect = std::make_shared<RouteEffectToken>(begun);
            }
            catch (...)
            {
                // Balance the recorder's call depth and in-flight counters; no End hook will run.
                EndRouteEffectDraw(begun, ctx);
                throw;
            }
        }
        return token;
    }
    catch (const std::exception& e)
    {
        if (payloadPushed)
            payloadStack.pop_back();
        if (writerPushed)
            writerStack.pop_back();
        s.Failure(e.what(), relevant);
        if (token.session)
            s.Release();
        return {};
    }
}

void EndSceneEntry(SceneHookToken token, const PPCContext& ctx) noexcept
{
    if (token.effect)
        EndRouteEffectDraw(*token.effect, ctx);
    if (!token.session)
        return;
    auto& s = *static_cast<Session*>(token.session);
    if (token.payload && !payloadStack.empty() && payloadStack.back() == token.payload.get())
        payloadStack.pop_back();
    if ((token.writer || token.suppressCopies) && !writerStack.empty() && writerStack.back() == token.writer.get())
        writerStack.pop_back();
    if (token.suppressCopies && !s.payloads)
    {
        s.Release();
        return;
    }
    try
    {
        const bool emit = !token.suppressCopies || token.payload->inWindow;
        auto       p    = std::make_unique<ScenePacket>();
        {
            std::lock_guard lock(s.gate);
            if (s.aborted || s.written)
            {
                --s.inflight;
                return;
            }
            p->sequence = emit ? ++s.sequence : token.payload->event;
            p->frame    = emit ? s.frame : token.payload->frame;
            p->entry    = token.entry;
            if (emit)
                ++s.events;
        }
        auto t = token.writer ? token.writer->metadata : toml::table{ { "kind", "creation" } };
        t.insert_or_assign("kind", token.writer ? "cpu_writer" : "creation");
        if (token.payload)
        {
            payloadIdentity(s, *token.payload, t);
            if (drawEntry(token.entry))
                t.insert_or_assign("kind", "draw_post");
            else if (token.entry == 0x8269E520)
                t.insert_or_assign("kind", "resolve_return");
            else if (token.suppressCopies)
                t.insert_or_assign("kind", "upload_return");
        }
        if (!token.writer)
            t.insert("parent_sequence", static_cast<std::int64_t>(token.parent));
        t.insert_or_assign("caller_lr", std::to_string(token.caller));
        t.insert("raw_return_r3", std::to_string(ctx.r3.u64));
        try
        {
            if (token.payload && (drawEntry(token.entry) || token.entry == 0x8269E520))
            {
                payloadAttempt(s, t, [&]
                               {
                                   std::lock_guard payloadLock(s.payloadGate);
                                   if (drawEntry(token.entry))
                                       postDrawPayloads(s, *token.payload, t);
                                   else
                                   {
                                       t.insert("completion_id", s.payloads->CaptureId() + ":resolve:" + std::to_string(token.payload->event));
                                       t.insert("completion_state", "guest_return_only_gpu_pending");
                                   }
                               });
            }
            else if (token.writer)
            {
                if (token.entry == 0x82305648)
                    t.insert("backend_after", word(memory(std::uint64_t(token.object) + 40, 4), 0));
                if (token.writer->directRegionPair && word(memory(std::uint64_t(token.object) + 40, 4), 0))
                {
                    const auto pair = memory(token.writer->directRegionPair, 8);
                    linearDestination(*token.writer, word(pair, 4), word(pair, 0), token.writer->directRegionRows, token.writer->directRegionRowBytes);
                }
                auto& destinations = token.writer->destinations;
                std::sort(destinations.begin(), destinations.end(), [](const auto& a, const auto& b)
                          {
                              return a.address < b.address;
                          });
                std::vector<SceneGuestRange> merged;
                for (const auto& r : destinations)
                {
                    if (!merged.empty() && r.address <= std::uint64_t(merged.back().address) + merged.back().size)
                        merged.back().size = static_cast<std::uint32_t>(std::max(std::uint64_t(merged.back().address) + merged.back().size,
                                                                                 std::uint64_t(r.address) + r.size) -
                                                                        merged.back().address);
                    else
                        merged.push_back(r);
                }
                // Propagate once, before any hash read can fail; the parent forwards its
                // merged union later.
                if (!writerStack.empty() && writerStack.back())
                    for (const auto& r : merged)
                    {
                        auto& parent = *writerStack.back();
                        if (parent.payloadOnly)
                            payloadAttempt(s, parent.metadata, [&]
                                           {
                                               destination(parent, r.address, r.size);
                                           });
                        else if (!token.writer->payloadOnly)
                            destination(parent, r.address, r.size);
                    }
                if (!token.writer->payloadOnly)
                {
                    toml::array ranges;
                    for (const auto& r : merged)
                    {
                        // Hash through the original virtual alias, after Original.
                        toml::table span{ { "guest_address", r.address }, { "bytes", r.size }, { "destination_sha256", digest(memory(r.address, r.size)) } };
                        if (r.address >= 0xA0000000U)
                        {
                            span.insert("begin", static_cast<std::int64_t>(physicalAddress(r.address)));
                            span.insert("end", static_cast<std::int64_t>(physicalAddress(r.address) + r.size));
                        }
                        ranges.push_back(std::move(span));
                    }
                    t.insert("destination_ranges", std::move(ranges));
                    t.insert("destination_open", merged.empty() || t.contains("read_error"));
                    t.insert("hash_extent", "union of derived linear rows and observed nested copy destinations; hashes after return");
                    if (token.entry == 0x826B5F30)
                        t.insert("return_status", ctx.r3.s32);
                }
                if (token.payload && s.payloads->Active())
                {
                    payloadAttempt(s, t, [&]
                                   {
                                       std::lock_guard payloadLock(s.payloadGate);
                                       auto            call = *token.payload;
                                       call.event           = p->sequence;
                                       call.frame           = p->frame;
                                       writerPayloads(s, call, *token.writer, merged, t);
                                   });
                }
            }
            else
            {
                header(t, token.entry == 0x826B7140 ? token.object : ctx.r3.u32, token.entry == 0x825184D0 ? 156 : token.entry == 0x82302DC8 ? 48
                                                                                                                                             : 52);
                std::vector<SceneGuestRange> ranges;
                if (const auto* tex = t["texture"].as_table())
                    for (const auto& pair : { std::pair{ "base", "base_bytes" }, std::pair{ "mip_base", "mip_bytes" } })
                    {
                        const auto address = (*tex)[pair.first].value_or(0U), size = (*tex)[pair.second].value_or(0U);
                        if (address && size)
                            ranges.push_back({ address, size });
                    }
                if (token.payload && s.payloads->Active())
                {
                    payloadAttempt(s, t, [&]
                                   {
                                       std::lock_guard payloadLock(s.payloadGate);
                                       auto            call = *token.payload;
                                       call.event           = p->sequence;
                                       call.frame           = p->frame;
                                       creationPayloads(s, call, t, ranges);
                                   });
                }
                if (!s.storage.Set(t["object"].value_or(0U), std::move(ranges)))
                {
                    std::lock_guard lock(s.gate);
                    ++s.capacityOverflow;
                    throw std::runtime_error("scene_texture_membership_capacity");
                }
            }
        }
        catch (const std::exception& e)
        {
            if (token.writer && token.writer->payloadOnly)
            {
                t.insert_or_assign("payload_error", e.what());
                ++s.payloadFailures;
            }
            else
            {
                const bool relevant = !token.writer || writerCapacityFailure(e.what());
                t.insert_or_assign("read_error", e.what());
                t.insert_or_assign("failure_relevant", relevant);
                if (token.writer)
                    t.insert_or_assign("destination_open", true);
                if (token.payload && (relevant || token.payload->inWindow || (token.writer && !token.writer->sourcePayloads.empty())))
                    ++s.payloadGaps;
                s.Failure(e.what(), relevant);
            }
        }
        if (emit)
            push(s, std::move(p), t);
    }
    catch (const std::exception& e)
    {
        s.Failure(e.what(), !token.writer);
    }
    s.Release();
}

bool StartSceneResourceCapture(const std::filesystem::path& directory, std::uint32_t frames, std::string_view title, std::string_view sdk, std::string& error, bool payloads, std::uint32_t payloadBytes, bool effects, bool targets, std::uint32_t targetBytes, std::uint32_t armFrame, const SceneCaptureTriggers& triggers)
{
    std::lock_guard lock(lifecycle);
    try
    {
        if (targets && (!payloads || !targetBytes))
            throw std::runtime_error("target capture requires payload identities and a nonzero byte budget");
        if (session || !frames || frames > 16 || !std::filesystem::create_directories(directory))
            throw std::runtime_error("scene capture needs a fresh directory, 1..16 frames, and one session");
        std::string pixelDigest;
        if (!triggers.pixelDigest.empty() && !NormalizeScenePixelDigest(triggers.pixelDigest, pixelDigest))
            throw std::runtime_error("the scene arm digest needs 8 to 64 hex digits of a pixel shader word digest");
        if (!triggers.windows || triggers.windows > 16 || (triggers.windows > 1 && (effects || targets)))
            throw std::runtime_error("scene windows are 1..16 and exclude route effects and targets");
        auto* s          = new Session(std::make_shared<Tracking>());
        s->directory     = directory;
        s->root          = directory;
        s->frames        = frames;
        s->armFrame      = armFrame;
        s->title         = title;
        s->sdk           = sdk;
        s->pixelDigest   = pixelDigest;
        s->armDelay      = triggers.armDelay;
        s->windows       = triggers.windows;
        s->payloadBudget = payloadBytes;
        if (payloads)
        {
            const auto identity = directory.generic_string() + std::string(title) + std::string(sdk) +
                                  std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            s->payloads         = std::make_unique<ScenePayloadStore>(true, payloadBytes, rex::crypto::sha256(identity));
            if (targets)
                s->targets = std::make_unique<SceneTargetReadback>(s->payloads->CaptureId(), targetBytes);
        }
        // Effect records join payload call and submission identities.
        if (effects && !payloads)
            throw std::runtime_error("route effects require scene payload identities");
        if (effects && !StartRouteEffects(directory, &memory, error))
            throw std::runtime_error(error);
        s->effects = effects;
        session    = s;
        sessions.push_back(s);
        s->worker = std::jthread([s](std::stop_token stop)
                                 {
                                     run(*s, stop);
                                 });
        sceneCaptureSession.store(s, std::memory_order_release);
        if (payloads)
            sceneCoverageSession.store(s, std::memory_order_release);
        if (!pixelDigest.empty() && !AddSceneDrawWatch(pixelDigest, onDigestHit, s, error))
            throw std::runtime_error(error);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

void NotifySceneResourceFrameBoundary() noexcept
{
    auto* raw = sceneCaptureSession.load(std::memory_order_relaxed);
    if (!raw || raw == &watchOnlyMarker)
        return;
    auto* s = static_cast<Session*>(raw);
    std::atomic_thread_fence(std::memory_order_acquire);
    bool successor = false;
    {
        std::unique_lock clock(watchClock, std::defer_lock);
        if (!s->pixelDigest.empty())
            clock.lock();
        std::lock_guard lock(s->gate);
        if (s->closing)
            return;
        ++s->frame;
        // The arm file, arm_frame and the pixel digest are independent triggers; whichever fires first opens the window.
        if (!s->arm && s->armFrame && s->frame >= s->armFrame)
        {
            s->arm     = true;
            s->trigger = "arm_frame";
        }
        if (!s->arm && s->digestDue && s->frame >= s->digestDue)
        {
            s->arm     = true;
            s->trigger = "pixel_digest";
        }
        if (!s->window && s->arm)
        {
            s->window = true;
            s->begin  = s->frame;
            if (!s->pixelDigest.empty())
                RemoveSceneDrawWatches(s);
            if (s->targets)
            {
                SceneTargetRecord r;
                r.kind  = "seed_scan";
                r.frame = s->frame;
                s->targets->Gap(std::move(r), "sdk_opening_footprint_readback_unavailable");
            }
        }
        else if (s->window)
        {
            if (s->targets)
            {
                SceneTargetRecord r;
                r.kind  = "reference";
                r.frame = s->frame - 1;
                s->targets->Gap(std::move(r), "sdk_ordered_present_readback_unavailable");
            }
            if (s->frame == s->begin + s->frames)
            {
                s->end     = s->frame;
                s->window  = false;
                s->closing = true;
                // The next window's session takes over observation; otherwise stop now.
                successor = s->windowIndex < s->windows;
                if (!successor)
                    releaseObservation(s, true);
            }
        }
        if (s->effects)
            SetRouteEffectWindow(s->window, s->frame);
    }
    if (successor)
        startSuccessor(*s);
}

void StopSceneResourceCapture() noexcept
{
    std::lock_guard lock(lifecycle);
    stopped = true;
    sceneCaptureSession.store(nullptr, std::memory_order_relaxed);
    sceneCoverageSession.store(nullptr, std::memory_order_relaxed);
    for (auto* s : sessions)
    {
        RemoveSceneDrawWatches(s);
        if (s->worker.joinable())
            s->worker.request_stop();
    }
    for (auto* s : sessions)
        if (s->worker.joinable())
            s->worker.join();
}

} // namespace rerevved::gpu::diagnostics

// Only the title executable links generated originals. Recorder tests exercise
// ObserveSceneAllocation directly without replacing guest allocation behavior.
#if defined(REREVVED_VERSION)
REX_EXTERN(__imp__sub_8280DAC0);
REX_EXTERN(__imp__sub_8280DB58);

REX_HOOK_RAW(sub_8280DAC0)
{
    using namespace rerevved::gpu::diagnostics;
    if (auto* s = sceneCoverageSession.load(std::memory_order_acquire))
    {
        const auto bytes = ctx.r3.u32, flags = ctx.r4.u32;
        __imp__sub_8280DAC0(ctx, base);
        ObserveSceneAllocation(s, false, ctx.r3.u32, bytes, flags);
        return;
    }
    __imp__sub_8280DAC0(ctx, base);
}

REX_HOOK_RAW(sub_8280DB58)
{
    using namespace rerevved::gpu::diagnostics;
    if (auto* s = sceneCoverageSession.load(std::memory_order_acquire))
        ObserveSceneAllocation(s, true, ctx.r3.u32, 0, ctx.r4.u32);
    __imp__sub_8280DB58(ctx, base);
}
#endif
