#include "indexed_buffer_draw_inputs.h"

#include "gpu/cached_sha256.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace rerevved::gpu::guest_d3d
{

// Here with the reader that takes them, so every target linking the adapter
// library has them.
namespace
{

thread_local std::vector<std::uint8_t> spareState;
// This thread's spare reader storage, emptied: ReturnDrawInputs keeps a
// draw's vectors here and the next ReadIndexedBufferDrawInputs reuses them.
thread_local IndexedBufferDrawInputs spareInputs;

// Applies `move(to, from)` to every vector of the inputs but the state, which
// the state buffer recycles. A vector missing here only loses its storage.
template <typename Move>
void eachInputVector(IndexedBufferDrawInputs& to, IndexedBufferDrawInputs& from, Move&& move)
{
    move(to.vertexCode, from.vertexCode);
    move(to.pixelCode, from.pixelCode);
    move(to.vertexMetadata, from.vertexMetadata);
    move(to.pixelMetadata, from.pixelMetadata);
    move(to.vertexHeader, from.vertexHeader);
    move(to.pixelHeader, from.pixelHeader);
    move(to.declarationHeader, from.declarationHeader);
    move(to.vertexLiterals, from.vertexLiterals);
    move(to.pixelLiterals, from.pixelLiterals);
    move(to.fetchTemplates, from.fetchTemplates);
    move(to.fetchRecords, from.fetchRecords);
    move(to.declaration, from.declaration);
    move(to.swizzleTable, from.swizzleTable);
    move(to.formatTable, from.formatTable);
    move(to.vertices, from.vertices);
    move(to.indices, from.indices);
    move(to.streams, from.streams);
    move(to.streamFetches, from.streamFetches);
    move(to.streamBytes, from.streamBytes);
    move(to.vertexLiteralRanges, from.vertexLiteralRanges);
    move(to.pixelLiteralRanges, from.pixelLiteralRanges);
    move(to.vertexLiteralRegisters, from.vertexLiteralRegisters);
    move(to.pixelLiteralRegisters, from.pixelLiteralRegisters);
    move(to.textureFetches, from.textureFetches);
    move(to.textureRanges, from.textureRanges);
    move(to.textureBaseBytes, from.textureBaseBytes);
    move(to.textureMipBytes, from.textureMipBytes);
    move(to.textureResolved, from.textureResolved);
}

// Resets `inputs` to a default value whose vectors keep the larger storage
// of its own and the spare's, emptied.
void resetInputs(IndexedBufferDrawInputs& inputs)
{
    inputs.indexBytes = {};
    if (inputs.state.capacity())
        ReturnStateBuffer(std::move(inputs.state));
    IndexedBufferDrawInputs fresh;
    eachInputVector(fresh, inputs, [](auto& to, auto& own)
                    {
                        own.clear();
                        to.swap(own);
                    });
    eachInputVector(fresh, spareInputs, [](auto& to, auto& spare)
                    {
                        if (spare.capacity() > to.capacity())
                        {
                            spare.clear();
                            to.swap(spare);
                        }
                    });
    inputs = std::move(fresh);
}

} // namespace

void ReturnDrawInputs(IndexedBufferDrawInputs&& inputs) noexcept
{
    inputs.indexBytes = {};
    ReturnStateBuffer(std::move(inputs.state));
    eachInputVector(spareInputs, inputs, [](auto& spare, auto& own)
                    {
                        if (own.capacity() > spare.capacity())
                        {
                            own.clear();
                            spare.swap(own);
                        }
                    });
}

std::vector<std::uint8_t> TakeStateBuffer() noexcept
{
    auto buffer = std::move(spareState);
    spareState  = {};
    buffer.clear();
    return buffer;
}

void ReturnStateBuffer(std::vector<std::uint8_t>&& buffer) noexcept
{
    if (buffer.capacity() > spareState.capacity())
        spareState = std::move(buffer);
}

namespace
{

constexpr std::uint64_t kMaxCodeBytes = 64 * 1024;
constexpr std::uint64_t kMaxGeometry  = 16 * 1024 * 1024;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("indexed_buffer_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

// The guest's physical alias calculation (0x826ADF14 for pixel shaders); the
// index buffer address converts the same way.
std::uint64_t physical(std::uint64_t raw, bool pixel = false)
{
    require(raw <= UINT32_MAX, "indexed_buffer_address_overflow");
    return (raw & (pixel ? 0x1FFFFFFEULL : 0x1FFFFFFFULL)) + (((raw >> 20) + 512) & 0x1000);
}

// 0x826AD8E8 links exports only for a mismatched, unskipped VS/PS pair.
bool needsLinkage(std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps)
{
    const auto vw = word(vs, 20), pw = word(ps, 20);
    return ((vw ^ pw) & 0x7FFFF) && (vs[8] & 7) != 7 && !(vw & 0x40000) && !(pw & 0x20000);
}

std::uint64_t codeBytes(std::span<const std::uint8_t> metadata)
{
    const auto size = word(metadata, 4);
    require(size && size % 4 == 0 && size <= kMaxCodeBytes, "indexed_buffer_microcode_extent");
    return size;
}

// GetNativeTextureMemoryRanges, which depends on the fetch words alone, from
// this thread's last answers for them.
bool textureMemoryRanges(const NativeTextureFetch& fetch, NativeTextureMemoryRanges& ranges, std::string& error)
{
    struct Answer
    {
        NativeTextureFetch        fetch{};
        bool                      known = false, ok = false;
        NativeTextureMemoryRanges ranges;
        std::string               error;
    };

    thread_local std::array<Answer, 64> answers;
    std::uint32_t                       hash = 0;
    for (const auto value : fetch)
        hash = (hash ^ value) * 0x9E3779B1U;
    auto& answer = answers[hash >> 26];
    if (!answer.known || answer.fetch != fetch)
    {
        answer.known = false;
        answer.fetch = fetch;
        answer.ok    = GetNativeTextureMemoryRanges(fetch, answer.ranges, answer.error);
        answer.known = true;
    }
    ranges = answer.ranges;
    if (!answer.ok)
        error = answer.error;
    return answer.ok;
}

// Per-thread mirror hints group caller, index object, shaders, declaration/form
// bind sites(index, streams, texture base/mips, fetch 16/Menu texture). Group
// key is no validation: a hint serves only an identical read.
class ReadPlans
{
public:
    struct Key
    {
        std::uint32_t     caller = 0, indexBuffer = 0, vertex = 0, pixel = 0, declaration = 0;
        IndexedBufferForm form                         = IndexedBufferForm::Prepass;
        bool              operator==(const Key&) const = default;
    };

    static constexpr std::size_t kIndexSite = 0, kStreamSite = 1, kTextureSite = 17, kVertexTextureSite = 17 + 2 * 32, kMenuTextureSite = kVertexTextureSite + 2;

    // The draw's hints while this lives; none for a nested read.
    explicit ReadPlans(const Key& key)
    {
        if (busy)
            return;
        if (plans.size() >= kMaxPlans && !plans.contains(key))
            plans.clear();
        hints = &plans[key];
        busy  = true;
    }

    ~ReadPlans()
    {
        if (hints)
            busy = false;
    }

    ReadPlans(const ReadPlans&)            = delete;
    ReadPlans& operator=(const ReadPlans&) = delete;

    // `read`'s bytes for `request` through the site's hint.
    GuestBytes Resource(const ReadMemory& read, GuestResourceRead request, std::size_t site) const
    {
        if (hints && read.mirroredResource)
        {
            if (hints->size() <= site)
                hints->resize(site + 1);
            request.hint = &(*hints)[site];
        }
        return read.Resource(request);
    }

private:
    struct KeyHash
    {
        std::size_t operator()(const Key& key) const noexcept
        {
            const auto objects = (std::uint64_t(key.vertex) << 32) | key.pixel;
            const auto rest    = (std::uint64_t(key.indexBuffer) << 32) | (key.declaration ^ (key.caller << 3) ^ std::uint32_t(key.form));
            return std::hash<std::uint64_t>{}(objects ^ (rest * 0x9E3779B97F4A7C15ULL));
        }
    };

    using Plans = std::unordered_map<Key, std::vector<GuestResourceBindHint>, KeyHash>;

    static constexpr std::size_t kMaxPlans = 1024;

    inline static thread_local Plans    plans;
    inline static thread_local bool     busy  = false;
    std::vector<GuestResourceBindHint>* hints = nullptr;
};

// The reader's resolved texture for a fetch at the draw's op mark, if any.
std::optional<NativeResolvedTexture> resolvedTexture(const ReadMemory& read, const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark)
{
    if (!read.resolvedTexture)
        return std::nullopt;
    auto miss = NativeResolvedTextureMiss::Unmodeled;
    return read.resolvedTexture(fetch, opMark, miss);
}

// Literal tables as 0x826ADB98 and 0x826ADEF8 consume them (label reader form).
void readLiterals(const ReadMemory& read, std::uint64_t headerAddress, std::span<const std::uint8_t> header, std::vector<std::uint8_t>& literals)
{
    const auto relative = word(header, 20);
    if (!relative)
        return;
    literals        = read(headerAddress + relative, 20, false);
    const auto size = word(literals, 16);
    require(size <= 128 && size % 8 == 0, "indexed_buffer_literal_table_extent");
    if (size)
        literals = read(headerAddress + relative, 20 + size, false);
}

// The pixel shader object's header, metadata and microcode.
void readPixelStage(const ReadMemory& read, IndexedBufferDrawInputs& inputs)
{
    inputs.pixelHeader = read(inputs.pixelObject, 68, false);
    require(inputs.pixelHeader.size() == 68, "indexed_buffer_shader_header_extent");
    const auto psHeader          = std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28);
    const auto psMetadataAddress = std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40;
    inputs.pixelMetadata         = read(psMetadataAddress, 32, false);
    {
        const GuestReadSiteScope site(GuestReadSite::ShaderCode);
        inputs.pixelCode = read(physical(std::uint64_t(word(inputs.pixelHeader, 24)) + word(inputs.pixelMetadata, 0), true), codeBytes(inputs.pixelMetadata), true);
    }
}

// With an active PS, 0x826ADD88 selects regular VS variant zero. Without one
// (0x826ADDF0..0x826ADE2C) it selects variant 1 when VS header word +872 has
// bit 0x20. 0x826ADA38 reads variant v's metadata offset at +896 + 8v.
void readVertexStage(const ReadMemory& read, IndexedBufferDrawInputs& inputs)
{
    inputs.vertexHeader = read(inputs.vertexObject, 912, false);
    require(inputs.vertexHeader.size() == 912, "indexed_buffer_shader_header_extent");
    inputs.variant               = !inputs.pixelObject && (word(inputs.vertexHeader, 872) & 0x20) ? 1 : 0;
    const auto vsHeader          = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
    const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24 + 8 * inputs.variant) + 872;
    inputs.vertexMetadata        = read(vsMetadataAddress, 32, false);
    {
        const GuestReadSiteScope site(GuestReadSite::ShaderCode);
        inputs.vertexCode = read(physical(std::uint64_t(word(inputs.vertexHeader, 32)) + word(inputs.vertexMetadata, 0)), codeBytes(inputs.vertexMetadata), true);
    }
}

// The VS fetch records, templates and any linkage lists, and the vertex
// declaration, after readPixelStage and readVertexStage: what the fold and
// the fetch patch read besides the code.
void readPatchInterface(const ReadMemory& read, IndexedBufferDrawInputs& inputs, bool linkage = true)
{
    const auto vsHeader          = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
    const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24 + 8 * inputs.variant) + 872;
    const auto psHeader          = linkage && inputs.pixelObject ? std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28) : std::span<const std::uint8_t>();
    const auto psMetadataAddress = linkage && inputs.pixelObject ? std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40 : 0;
    const auto fetchCount        = word(inputs.vertexMetadata, 28);
    require(fetchCount <= 32, "indexed_buffer_fetch_record_extent");
    const auto recordsOffset = (std::uint64_t(word(inputs.vertexMetadata, 24)) + 9) * 4;
    require(recordsOffset <= kMaxCodeBytes, "indexed_buffer_fetch_record_extent");
    if (fetchCount)
    {
        inputs.fetchRecords   = read(vsMetadataAddress + recordsOffset, fetchCount * 4, false);
        inputs.fetchTemplates = read(std::uint64_t(inputs.vertexObject) + 68, fetchCount * 12, false);
    }
    const bool link          = linkage && inputs.pixelObject && needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
    const auto vsCount       = link ? (word(inputs.vertexMetadata, 20) >> 5) & 31 : 0;
    const auto psCount       = link ? (word(inputs.pixelMetadata, 20) >> 5) & 31 : 0;
    const auto linkageOffset = recordsOffset + fetchCount * 4;
    require(linkageOffset + vsCount * 4 <= kMaxCodeBytes, "indexed_buffer_linkage_metadata_extent");
    inputs.vertexMetadata = read(vsMetadataAddress, std::max<std::uint64_t>(32, linkageOffset + vsCount * 4), false);
    if (linkage && inputs.pixelObject)
        inputs.pixelMetadata = read(psMetadataAddress, 32 + psCount * 4, false);
    const auto listsOffset = linkageOffset + vsCount * 4;
    for (unsigned i = 0; i < vsCount; ++i)
    {
        const auto listIndex  = (word(inputs.vertexMetadata, linkageOffset + i * 4) >> 16) & 0xFFF;
        bool       terminated = false;
        for (unsigned j = 0; j < 4096; ++j)
        {
            const auto offset = listsOffset + (listIndex + j) * 4;
            require(offset + 4 <= kMaxCodeBytes, "indexed_buffer_linkage_metadata_extent");
            if (offset + 4 > inputs.vertexMetadata.size())
                inputs.vertexMetadata = read(vsMetadataAddress, offset + 4, false);
            if (word(inputs.vertexMetadata, offset) & 0x1000)
            {
                terminated = true;
                break;
            }
        }
        require(terminated, "indexed_buffer_linkage_patch_list_unterminated");
    }

    require(inputs.declarationAddress, "indexed_buffer_missing_declaration");
    inputs.declarationHeader = read(inputs.declarationAddress, 52, false);
    const auto elements      = word(inputs.declarationHeader, 24);
    require(elements <= 32, "indexed_buffer_declaration_extent");
    if (elements)
        inputs.declaration = read(std::uint64_t(inputs.declarationAddress) + 52, elements * 12, false);
    inputs.swizzleTable = read(0x82091394, 16, false);
    inputs.formatTable  = read(0x820913A8, 64, false);
}

// Both stages' literals, the VS fetch records, templates and any linkage
// lists, and the vertex declaration, after readPixelStage and readVertexStage.
// Without a pixel shader only the vertex stage's are read.
void readShaderInterface(const ReadMemory& read, IndexedBufferDrawInputs& inputs, IndexedBufferForm form)
{
    const auto vsHeader = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
    const auto psHeader = inputs.pixelObject ? std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28) : std::span<const std::uint8_t>();
    // A vertex-only draw, read as the Leader or Scene form, has no PS records
    // and no linkage. Its VS literal records are walked as with a PS bound;
    // their layout under variant 1 is unverified.
    if (!inputs.pixelObject)
    {
        require(form == IndexedBufferForm::Leader || form == IndexedBufferForm::Scene, "indexed_buffer_missing_pixel_shader");
        std::string literalError;
        if (!ReadShaderResourceLiterals(read, inputs.vertexObject, false, inputs.vertexLiteralRanges, inputs.vertexLiteralRegisters, literalError))
            throw std::runtime_error("indexed_buffer_literals: " + literalError);
    }
    else if (form == IndexedBufferForm::Prepass)
    {
        readLiterals(read, std::uint64_t(inputs.vertexObject) + 872, vsHeader, inputs.vertexLiterals);
        readLiterals(read, std::uint64_t(inputs.pixelObject) + 40, psHeader, inputs.pixelLiterals);
    }
    else
    {
        // 0x826ADB98's record walk; the raw form above refuses a table whose
        // byte count is not a multiple of 8, as the menu VS table's is.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pixelRegisters;
        std::string                                          literalError;
        if (!ReadShaderResourceLiterals(read, inputs.vertexObject, false, inputs.vertexLiteralRanges, inputs.vertexLiteralRegisters, literalError) ||
            !ReadShaderResourceLiterals(read, inputs.pixelObject, true, inputs.pixelLiteralRanges, pixelRegisters, literalError))
            throw std::runtime_error("indexed_buffer_literals: " + literalError);
        if (form == IndexedBufferForm::Leader || form == IndexedBufferForm::SceneLeader || form == IndexedBufferForm::Scene)
            inputs.pixelLiteralRegisters = std::move(pixelRegisters);
    }

    readPatchInterface(read, inputs);
}

// The shader objects the device shadow at `device` names, from two words; a
// vertex-only draw names no pixel shader.
bool readShaderObjects(std::uint32_t device, const ReadMemory& read, IndexedBufferDrawInputs& inputs)
{
    const auto objects  = read(std::uint64_t(device) + 0x3194, 8, false);
    inputs.pixelObject  = word(objects, 0);
    inputs.vertexObject = word(objects, 4);
    return inputs.vertexObject != 0;
}

// A guest read a cache entry was built from, with the bytes it returned.
struct JournaledRead
{
    std::uint64_t             address = 0, size = 0;
    bool                      physical = false;
    std::vector<std::uint8_t> bytes;
};

using ReadJournal = std::vector<JournaledRead>;

// `read`, appending each read to `journal`.
ReadMemory journaled(const ReadMemory& read, ReadJournal& journal)
{
    ReadMemory recorder                     = read;
    static_cast<GuestMemoryRead&>(recorder) = [&read, &journal](std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto bytes = read(address, size, physical);
        journal.push_back({ address, size, physical, std::vector<std::uint8_t>(bytes.begin(), bytes.end()) });
        return bytes;
    };
    return recorder;
}

ReadMemory countedShaderReads(const ReadMemory& read)
{
    ReadMemory counted                     = read;
    static_cast<GuestMemoryRead&>(counted) = [&read](std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto bytes = read(address, size, physical);
        CountVertexShaderIdentityBytes(bytes.size());
        return bytes;
    };
    return counted;
}

// Whether every journaled read returned all the bytes it asked for; a failed
// read, such as one under a resolve image without its readback, returns
// fewer and is not kept.
bool complete(const ReadJournal& journal)
{
    return std::ranges::all_of(journal, [](const JournaledRead& entry)
                               {
                                   return entry.bytes.size() == entry.size;
                               });
}

// Whether guest memory still holds every journaled read's bytes, in full.
bool current(const ReadJournal& journal, const GuestMemoryView& view)
{
    return std::ranges::all_of(journal, [&](const JournaledRead& entry)
                               {
                                   const auto bytes = view(entry.address, entry.size, entry.physical);
                                   return bytes.size() == entry.size && std::ranges::equal(bytes, entry.bytes);
                               });
}

} // namespace

struct GuestShaderReadCache::Impl
{
    struct PixelStage
    {
        ReadJournal               journal;
        std::vector<std::uint8_t> header, metadata, code;
        std::string               digest;
    };

    // What readVertexStage then readShaderInterface for `form` read.
    struct InterfaceKey
    {
        std::uint32_t     vertex = 0, pixel = 0, declaration = 0;
        IndexedBufferForm form                                  = IndexedBufferForm::Prepass;
        bool              operator==(const InterfaceKey&) const = default;
    };

    struct InterfaceKeyHash
    {
        std::size_t operator()(const InterfaceKey& key) const noexcept
        {
            const auto objects = (std::uint64_t(key.vertex) << 32) | key.pixel;
            const auto rest    = (std::uint64_t(key.declaration) << 8) | std::uint64_t(key.form);
            return std::hash<std::uint64_t>{}(objects ^ (rest * 0x9E3779B97F4A7C15ULL));
        }
    };

    // Also valid only for the pixel header and metadata it was read with,
    // which locate the pixel stage's records.
    struct Interface
    {
        ReadJournal               journal;
        std::vector<std::uint8_t> pixelHeader, pixelMetadata;
        IndexedBufferDrawInputs   shader; // the fields both reads set
    };

    // Entries a map keeps before it starts over.
    static constexpr std::size_t kMaxEntries = 4096;

    template <typename Map>
    typename Map::mapped_type find(Map& map, const typename Map::key_type& key, const GuestMemoryView& view)
    {
        typename Map::mapped_type entry;
        {
            std::lock_guard lock(mutex);
            if (const auto found = map.find(key); found != map.end())
                entry = found->second;
        }
        const bool      fresh = entry && current(entry->journal, view);
        std::lock_guard lock(mutex);
        ++(fresh ? counts.hits : entry ? counts.stale
                                       : counts.misses);
        return fresh ? entry : nullptr;
    }

    template <typename Map>
    void keep(Map& map, const typename Map::key_type& key, typename Map::mapped_type entry)
    {
        if (!complete(entry->journal))
            return;
        std::lock_guard lock(mutex);
        if (map.size() >= kMaxEntries && !map.contains(key))
            map.clear();
        map[key] = std::move(entry);
    }

    std::mutex                                                                           mutex;
    std::unordered_map<std::uint32_t, std::shared_ptr<const PixelStage>>                 pixels;
    std::unordered_map<InterfaceKey, std::shared_ptr<const Interface>, InterfaceKeyHash> interfaces;
    std::unordered_map<InterfaceKey, std::shared_ptr<const Interface>, InterfaceKeyHash> patchInterfaces;
    Counts                                                                               counts;
};

GuestShaderReadCache::GuestShaderReadCache()
: impl(std::make_unique<Impl>())
{
}

GuestShaderReadCache::~GuestShaderReadCache() = default;

GuestShaderReadCache::Counts GuestShaderReadCache::TakeCounts() noexcept
{
    std::lock_guard lock(impl->mutex);
    return std::exchange(impl->counts, {});
}

namespace
{

GuestShaderReadCache::Impl* shaderCache(const ReadMemory& read) noexcept
{
    return read.shaderCache && read.view ? &read.shaderCache->State() : nullptr;
}

// readPixelStage and the pixel microcode's word digest, from the cache when
// guest memory still holds what its entry read.
std::string readPixelStageDigest(const ReadMemory& read, IndexedBufferDrawInputs& inputs)
{
    auto* cache = shaderCache(read);
    if (!cache)
    {
        readPixelStage(countedShaderReads(read), inputs);
        return IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
    }
    if (const auto entry = cache->find(cache->pixels, inputs.pixelObject, read.view))
    {
        inputs.pixelHeader   = entry->header;
        inputs.pixelMetadata = entry->metadata;
        inputs.pixelCode     = entry->code;
        return entry->digest;
    }
    auto       entry   = std::make_shared<GuestShaderReadCache::Impl::PixelStage>();
    const auto counted = countedShaderReads(read);
    readPixelStage(journaled(counted, entry->journal), inputs);
    entry->digest   = IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
    entry->header   = inputs.pixelHeader;
    entry->metadata = inputs.pixelMetadata;
    entry->code     = inputs.pixelCode;
    auto digest     = entry->digest;
    cache->keep(cache->pixels, inputs.pixelObject, std::move(entry));
    return digest;
}

// The fields readVertexStage and readShaderInterface set.
void adoptShaderInterface(const IndexedBufferDrawInputs& from, IndexedBufferDrawInputs& to)
{
    to.variant                = from.variant;
    to.vertexHeader           = from.vertexHeader;
    to.vertexMetadata         = from.vertexMetadata;
    to.vertexCode             = from.vertexCode;
    to.pixelMetadata          = from.pixelMetadata;
    to.vertexLiterals         = from.vertexLiterals;
    to.pixelLiterals          = from.pixelLiterals;
    to.vertexLiteralRanges    = from.vertexLiteralRanges;
    to.pixelLiteralRanges     = from.pixelLiteralRanges;
    to.vertexLiteralRegisters = from.vertexLiteralRegisters;
    to.pixelLiteralRegisters  = from.pixelLiteralRegisters;
    to.fetchRecords           = from.fetchRecords;
    to.fetchTemplates         = from.fetchTemplates;
    to.declarationHeader      = from.declarationHeader;
    to.declaration            = from.declaration;
    to.swizzleTable           = from.swizzleTable;
    to.formatTable            = from.formatTable;
}

} // namespace

bool ReadDrawShaderCode(std::uint32_t device, const ReadMemory& read, IndexedBufferDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    try
    {
        if (!readShaderObjects(device, read, inputs))
            return false;
        if (inputs.pixelObject)
            readPixelStage(read, inputs);
        readVertexStage(read, inputs);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ReadDrawShaderInputs(std::uint32_t device, const ReadMemory& read, IndexedBufferDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    try
    {
        inputs.state = read(device, 0x3500, false);
        require(inputs.state.size() == 0x3500, "indexed_buffer_shadow_extent");
        inputs.vertexObject       = word(inputs.state, 0x3198);
        inputs.pixelObject        = word(inputs.state, 0x3194);
        inputs.declarationAddress = word(inputs.state, 0x2E2C);
        if (!inputs.vertexObject)
            return false;
        if (inputs.pixelObject)
            readPixelStage(read, inputs);
        readVertexStage(read, inputs);
        readShaderInterface(read, inputs, IndexedBufferForm::Leader);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::string IndexedBufferMicrocodeWordDigest(std::span<const std::uint8_t> bytes)
{
    require(bytes.size() % 4 == 0, "indexed_buffer_microcode_extent");
    return gpu::CachedSha256::OfSwappedWords(bytes);
}

bool ReadHostPatchInputs(std::uint32_t device, std::uint32_t stride, const ReadMemory& read, IndexedBufferDrawInputs& inputs, bool& patches, std::string& error, bool diagnostic)
{
    inputs  = {};
    patches = false;
    error.clear();
    try
    {
        // Identity needs only bindings, never constants, PS code, or a shadow copy.
        ReadMemory identityRead                     = read;
        static_cast<GuestMemoryRead&>(identityRead) = [&read, diagnostic](std::uint64_t address, std::uint64_t size, bool physical)
        {
            // Content validation borrows bytes in place; count only actual copies.
            if (read.view)
            {
                const auto bytes = read.view(address, size, physical);
                if (bytes.size() == size)
                {
                    if (!diagnostic)
                        CountVertexShaderIdentityBytes(bytes.size());
                    return GuestBytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
                }
            }
            auto bytes = read(address, size, physical);
            if (!diagnostic)
                CountVertexShaderIdentityBytes(bytes.size());
            return bytes;
        };
        GuestBytes heldObjects, heldDeclaration;
        auto       objects = read.view ? read.view(std::uint64_t(device) + 0x3194, 8, false) : std::span<const std::uint8_t>();
        if (objects.size() != 8)
        {
            heldObjects = identityRead(std::uint64_t(device) + 0x3194, 8, false);
            objects     = heldObjects;
        }
        require(objects.size() == 8, "indexed_buffer_shadow_extent");
        inputs.pixelObject  = word(objects, 0);
        inputs.vertexObject = word(objects, 4);
        auto declaration    = read.view ? read.view(std::uint64_t(device) + 0x2E2C, 4, false) : std::span<const std::uint8_t>();
        if (declaration.size() != 4)
        {
            heldDeclaration = identityRead(std::uint64_t(device) + 0x2E2C, 4, false);
            declaration     = heldDeclaration;
        }
        require(declaration.size() == 4, "indexed_buffer_shadow_extent");
        inputs.declarationAddress = word(declaration, 0);
        inputs.arguments.stride   = stride;
        require(inputs.vertexObject, "indexed_buffer_shader_object_unset");
        auto*                                          cache = diagnostic ? nullptr : shaderCache(read);
        const GuestShaderReadCache::Impl::InterfaceKey key{ inputs.vertexObject, inputs.pixelObject, inputs.declarationAddress, IndexedBufferForm::Scene };
        if (cache)
            if (const auto entry = cache->find(cache->patchInterfaces, key, read.view))
            {
                adoptShaderInterface(entry->shader, inputs);
                inputs.pixelHeader = entry->shader.pixelHeader;
                patches            = inputs.declarationAddress && word(inputs.vertexMetadata, 28);
                return true;
            }
        auto       entry  = std::make_shared<GuestShaderReadCache::Impl::Interface>();
        const auto source = cache ? journaled(identityRead, entry->journal) : identityRead;
        if (inputs.pixelObject && !diagnostic)
        {
            inputs.pixelHeader = source(inputs.pixelObject, 68, false);
            require(inputs.pixelHeader.size() == 68, "indexed_buffer_shader_header_extent");
            inputs.pixelMetadata = source(std::uint64_t(inputs.pixelObject) + word(inputs.pixelHeader, 64) + 40, 32, false);
            codeBytes(inputs.pixelMetadata);
        }
        readVertexStage(source, inputs);
        // The patch needs a declaration and the shader's fetch records; without
        // either the guest leaves the words as they are.
        if (inputs.declarationAddress && word(inputs.vertexMetadata, 28))
        {
            readPatchInterface(source, inputs, !diagnostic);
            patches = true;
        }
        if (cache)
        {
            adoptShaderInterface(inputs, entry->shader);
            entry->shader.pixelHeader = inputs.pixelHeader;
            cache->keep(cache->patchInterfaces, key, std::move(entry));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ReadIndexedBufferDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, std::string_view pixelWordDigest, IndexedBufferDrawInputs& inputs, std::string& error, IndexedBufferForm form)
{
    const std::array<IndexedBufferPixelShader, 1> pixelShaders{ { { pixelWordDigest, 0 } } };
    return ReadIndexedBufferDrawInputs(submission, read, pixelShaders, inputs, error, form);
}

bool ReadIndexedBufferDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, std::span<const IndexedBufferPixelShader> pixelShaders, IndexedBufferDrawInputs& inputs, std::string& error, IndexedBufferForm form, std::span<const std::string_view> depthOnlyVertexShaders, const PatchedVertexDigest& patchedVertexDigest)
{
    resetInputs(inputs);
    error.clear();
    const bool particle = form == IndexedBufferForm::Particle;
    const bool scene    = form == IndexedBufferForm::Scene || particle;
    const bool leader   = form == IndexedBufferForm::Leader || form == IndexedBufferForm::SceneLeader || scene;
    const auto caller   = form == IndexedBufferForm::Leader ? kLeaderDrawCaller : (particle ? kParticleDrawCaller : kSceneMeshDrawCaller);
    const bool ours     = form == IndexedBufferForm::Scene ? std::ranges::find(kSceneDrawCallers, submission.caller) != kSceneDrawCallers.end() : submission.caller == caller;
    if (submission.hook != (particle ? NativeDrawHook::IndexedUpLowerer : NativeDrawHook::IndexedBuffer) || !ours)
        return false;
    // State uses the device shadow, or inputs.state when the shadow had to be
    // read; Scene draws retain it for reuse.
    std::span<const std::uint8_t> state;
    const auto                    keepState = [&]
    {
        if (inputs.state.empty() && state.size() == 0x3500)
        {
            inputs.state = TakeStateBuffer();
            inputs.state.assign(state.begin(), state.end());
            state = inputs.state;
        }
    };
    try
    {
        // r3..r7 at entry: device, primitive, base vertex, start index, index
        // count; the lowerer's r6 is its vertex count and its start index 0.
        const auto& r     = submission.arguments;
        inputs.primitive  = r[1];
        inputs.baseVertex = r[2];
        inputs.startIndex = particle ? 0 : r[3];
        inputs.indexCount = r[4];
        if (particle)
        {
            require(submission.entryState && submission.entryState->size() == 0x3500, "particle_state_unread");
            state = *submission.entryState;
        }
        else if (const auto shadow = read.view ? read.view(r[0], 0x3500, false) : std::span<const std::uint8_t>(); shadow.size() == 0x3500)
            state = shadow;
        else
        {
            inputs.state = read(r[0], 0x3500, false);
            state        = inputs.state;
        }
        require(state.size() == 0x3500, "indexed_buffer_shadow_extent");
        if (scene)
            keepState();
        inputs.vertexObject       = word(state, 0x3198);
        inputs.pixelObject        = word(state, 0x3194);
        inputs.declarationAddress = word(state, 0x2E2C);
        if (!inputs.vertexObject || (!inputs.pixelObject && (!scene || depthOnlyVertexShaders.empty())))
            return false;
        const ReadPlans plan({ static_cast<std::uint32_t>(submission.caller), particle ? 0 : word(state, 0x3094), inputs.vertexObject, inputs.pixelObject, inputs.declarationAddress, form });
        std::uint32_t   textureFetches = 0;
        bool            vertexTexture  = false;
        // The vertex stage and shader interface come from the cache's entry
        // while guest memory holds what it read; otherwise `shaderRead`
        // journals the reads that build a new one.
        using Interface                                           = GuestShaderReadCache::Impl::Interface;
        auto* const                                    cache      = shaderCache(read);
        const auto                                     shaderForm = particle ? IndexedBufferForm::Scene : form;
        const GuestShaderReadCache::Impl::InterfaceKey key{ inputs.vertexObject, inputs.pixelObject, inputs.declarationAddress, shaderForm };
        std::shared_ptr<const Interface>               cached;
        std::shared_ptr<Interface>                     building;
        std::optional<ReadMemory>                      journaling;
        const auto                                     countedRead   = countedShaderReads(read);
        bool                                           interfaceRead = false;
        const auto                                     shaderRead    = [&]() -> const ReadMemory&
        {
            if (!cache)
                return countedRead;
            if (!journaling)
            {
                building = std::make_shared<Interface>();
                journaling.emplace(journaled(countedRead, building->journal));
            }
            return *journaling;
        };
        const auto vertexStage = [&]
        {
            if (cache)
            {
                cached = cache->find(cache->interfaces, key, read.view);
                if (cached && cached->pixelHeader == inputs.pixelHeader && cached->pixelMetadata == inputs.pixelMetadata)
                {
                    adoptShaderInterface(cached->shader, inputs);
                    return;
                }
                cached.reset();
                shaderRead();
                building->pixelHeader   = inputs.pixelHeader;
                building->pixelMetadata = inputs.pixelMetadata;
            }
            readVertexStage(shaderRead(), inputs);
        };
        if (inputs.pixelObject)
        {
            const auto pixelDigest = readPixelStageDigest(read, inputs);
            const auto pixel       = std::ranges::find(pixelShaders, std::string_view(pixelDigest), &IndexedBufferPixelShader::wordDigest);
            if (pixel == pixelShaders.end())
                return false;
            inputs.pixelShader    = static_cast<std::size_t>(pixel - pixelShaders.begin());
            textureFetches        = pixel->textureFetches;
            vertexTexture         = scene && pixel->vertexTexture;
            inputs.sceneCandidate = true;
            keepState();
            vertexStage();
        }
        else
        {
            // A depth-only draw: no PS stage, and a VS from the list as this
            // draw's fetch patch leaves it. The words in guest memory depend on
            // whether the guest patched for this state, so they never decide.
            vertexStage();
            const auto listed = [&](const std::string& digest)
            {
                return std::ranges::find(depthOnlyVertexShaders, std::string_view(digest)) != depthOnlyVertexShaders.end();
            };
            // A cached entry's fields already hold the interface.
            try
            {
                if (!cached)
                {
                    readShaderInterface(shaderRead(), inputs, IndexedBufferForm::Scene);
                    interfaceRead = true;
                }
            }
            catch (const std::exception&)
            {
                return false;
            }
            if (!patchedVertexDigest || !listed(patchedVertexDigest(inputs, state[0x30F0] * 4U)))
            {
                // Only the name: listed words as read still report the fetch patch the draw does not match.
                if (patchedVertexDigest && listed(IndexedBufferMicrocodeWordDigest(inputs.vertexCode)))
                    error = "scene_depth_only_fetch_patch";
                return false;
            }
            inputs.sceneCandidate = true;
        }
        if (!cached)
        {
            if (!interfaceRead)
                readShaderInterface(shaderRead(), inputs, shaderForm);
            if (cache)
            {
                adoptShaderInterface(inputs, building->shader);
                cache->keep(cache->interfaces, key, std::move(building));
            }
        }
        const auto elements = word(inputs.declarationHeader, 24);

        std::uint32_t width = 2;
        if (particle)
        {
            // The commit published what the lowerer reserved at 0x826A3524
            // and the caller filled through its out slots ([r1+84] and r10).
            const auto& commit = submission.commit;
            require(commit && commit->read, "particle_geometry_unread");
            // Commit checks G+0x3484 at return/commit, outputs vs
            // G+0x3488/G+0x348C, and state G+0x3198/G+0x3194/G+0x2E2C. Changes
            // refuse instead of using later caller-memory state.
            require(commit->pendingEnd == commit->commitEnd, "particle_geometry_pending_end");
            require(commit->vertexOut == commit->vertexDest && commit->indexOut == commit->indexDest, "particle_geometry_dest");
            require(commit->vertexShader == submission.vertexShader && commit->pixelShader == submission.pixelShader &&
                        commit->declaration == submission.declaration,
                    "particle_state_changed");
            // r8 bit 2 selects 32-bit indices (r7 dwords, else (r7 + 1) / 2);
            // bits 0..1 are the endian. r6 * r9 / 4 vertex dwords.
            inputs.indexFormat = (r[5] >> 2) & 1;
            inputs.indexEndian = r[5] & 3;
            width              = inputs.indexFormat ? 4U : 2U;
            require(r[6] && r[6] % 4 == 0 && r[6] / 4 <= 0xFF && std::uint64_t(r[3]) * r[6] / 4 == commit->vertexDwords &&
                        commit->indexDwords == (inputs.indexFormat ? r[4] : (std::uint64_t(r[4]) + 1) / 2),
                    "particle_geometry_size");
            require(inputs.indexCount && std::uint64_t(inputs.indexCount) * width <= kMaxGeometry && std::uint64_t(commit->vertexDwords) * 4 <= kMaxGeometry,
                    "particle_geometry_size");
            inputs.indexAddress = static_cast<std::uint32_t>(physical(commit->indexDest));
            inputs.indexBytes   = read(commit->indexDest, std::uint64_t(inputs.indexCount) * width, false);
            require(indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * width, "particle_geometry_extent");
        }
        else
        {
            // SetIndices (0x8268FCB0) leaves the index buffer object at G+0x3094:
            // flags (+0) bit 31 selects 32-bit indices, bits 29..30 the endian;
            // +0x18 is the CPU address of index zero.
            inputs.indexBuffer = word(state, 0x3094);
            require(inputs.indexBuffer, "indexed_buffer_index_buffer_unbound");
            const auto header  = read(inputs.indexBuffer, 32, false);
            const auto flags   = word(header, 0);
            width              = (flags & 0x80000000U) ? 4U : 2U;
            inputs.indexFormat = width == 4 ? 1 : 0;
            inputs.indexEndian = (flags >> 29) & 3;
            require(inputs.indexCount && std::uint64_t(inputs.indexCount) * width <= kMaxGeometry, "indexed_buffer_index_extent");
            inputs.indexAddress = static_cast<std::uint32_t>(physical(std::uint64_t(word(header, 24)) + std::uint64_t(inputs.startIndex) * width));
            inputs.indexBytes   = plan.Resource(read, { GuestResourceKind::IndexBuffer, inputs.indexAddress, static_cast<std::uint32_t>(std::uint64_t(inputs.indexCount) * width), inputs.indexBuffer, { word(header, 24), word(header, 28) } }, ReadPlans::kIndexSite);
            require(indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * width, "indexed_buffer_index_extent");
        }

        // SetStreamSource (0x8268FB08) binds stream i as vertex fetch 95 - i;
        // its stride is the shadow byte G+0x30F0 + i, in words.
        for (std::uint32_t element = 0; element < elements; ++element)
        {
            const auto stream = word(inputs.declaration, element * 12) >> 16;
            require(stream < 16, "indexed_buffer_stream_extent");
            if (std::find(inputs.streams.begin(), inputs.streams.end(), stream) == inputs.streams.end())
                inputs.streams.push_back(stream);
        }
        std::sort(inputs.streams.begin(), inputs.streams.end());
        if (particle && inputs.streams == std::vector<std::uint32_t>{ 0 })
        {
            // 0x826A3334 emits fetch 95 only into PM4, so the shadow's is
            // stale: {physical | 3, 0x10000002 | dwords << 2}.
            const auto& commit = *submission.commit;
            inputs.streamFetches.push_back({ static_cast<std::uint32_t>(physical(commit.vertexDest)) | 3U, 0x10000002U | ((commit.vertexDwords << 2) & 0x03FFFFFCU) });
            inputs.streamBytes.push_back(read(commit.vertexDest, std::uint64_t(commit.vertexDwords) * 4, false));
            require(inputs.streamBytes.back().size() == std::uint64_t(commit.vertexDwords) * 4, "particle_geometry_extent");
        }
        for (const auto stream : particle ? std::vector<std::uint32_t>{} : inputs.streams)
        {
            const std::array<std::uint32_t, 2> fetch{ word(state, 0x480 + (95 - stream) * 8), word(state, 0x484 + (95 - stream) * 8) };
            require((fetch[0] & 3) == 3, "indexed_buffer_vertex_fetch_type");
            const auto size = std::uint64_t((fetch[1] >> 2) & 0xFFFFFF) * 4;
            require(size && size <= kMaxGeometry, "indexed_buffer_stream_extent");
            inputs.streamFetches.push_back(fetch);
            inputs.streamBytes.push_back(plan.Resource(read, { GuestResourceKind::VertexBuffer, fetch[0] & ~3U, static_cast<std::uint32_t>(size), 0, { fetch[0], fetch[1] } }, ReadPlans::kStreamSite + stream));
            require(inputs.streamBytes.back().size() == size, "indexed_buffer_stream_extent");
        }
        if (form == IndexedBufferForm::Menu)
        {
            // Snapshot texture fetch 0's base level as the shadow names it.
            for (std::size_t i = 0; i < inputs.textureFetch.size(); ++i)
                inputs.textureFetch[i] = word(state, 0x480 + i * 4);
            NativeTextureMemoryRange range;
            std::string              textureError;
            if (!GetNativeTextureMemoryRange(inputs.textureFetch, range, textureError))
                throw std::runtime_error("indexed_buffer_texture_range: " + textureError);
            inputs.textureAddress = range.address;
            inputs.textureBytes   = plan.Resource(read, { GuestResourceKind::Texture, range.address, range.size, 0, inputs.textureFetch }, ReadPlans::kMenuTextureSite);
            require(inputs.textureBytes.size() == range.size, "indexed_buffer_texture_extent");
        }
        if (leader)
        {
            // Snapshot each sampled fetch's base level and mip storage.
            require(textureFetches <= 32, "indexed_buffer_texture_range: fetch slot extent");
            for (std::uint32_t slot = 0; slot < textureFetches; ++slot)
            {
                auto& fetch = inputs.textureFetches.emplace_back();
                for (std::size_t i = 0; i < fetch.size(); ++i)
                    fetch[i] = word(state, 0x480 + slot * 24 + i * 4);
                auto&       ranges = inputs.textureRanges.emplace_back();
                std::string textureError;
                if (!textureMemoryRanges(fetch, ranges, textureError))
                    throw std::runtime_error("indexed_buffer_texture_range: " + textureError);
                // A resolved texture replaces the slot's bytes; a miss reads them.
                const auto& resolved = inputs.textureResolved.emplace_back(resolvedTexture(read, fetch, submission.operationMark));
                std::size_t site     = ReadPlans::kTextureSite + slot * 2;
                for (const auto& [range, bytes] : { std::pair{ ranges.base, &inputs.textureBaseBytes }, std::pair{ ranges.mips, &inputs.textureMipBytes } })
                {
                    bytes->push_back(range.size && !resolved ? plan.Resource(read, { GuestResourceKind::Texture, range.address, range.size, 0, fetch }, site) : GuestBytes{});
                    ++site;
                    require(resolved || bytes->back().size() == range.size, "indexed_buffer_texture_extent");
                }
            }
            if (vertexTexture)
            {
                // Fetch 16 may be stale when the drawn VS does not sample it,
                // so storage it fails to name leaves empty bytes, not a refusal.
                auto& fetch = inputs.textureFetches.emplace_back();
                for (std::size_t i = 0; i < fetch.size(); ++i)
                    fetch[i] = word(state, 0x480 + 16 * 24 + i * 4);
                auto&       ranges = inputs.textureRanges.emplace_back();
                std::string textureError;
                GuestBytes  base, mips;
                if (textureMemoryRanges(fetch, ranges, textureError))
                {
                    base = ranges.base.size ? plan.Resource(read, { GuestResourceKind::Texture, ranges.base.address, ranges.base.size, 0, fetch }, ReadPlans::kVertexTextureSite) : GuestBytes{};
                    mips = ranges.mips.size ? plan.Resource(read, { GuestResourceKind::Texture, ranges.mips.address, ranges.mips.size, 0, fetch }, ReadPlans::kVertexTextureSite + 1) : GuestBytes{};
                    if (base.size() != ranges.base.size || mips.size() != ranges.mips.size)
                    {
                        base.clear();
                        mips.clear();
                    }
                }
                inputs.textureBaseBytes.push_back(std::move(base));
                inputs.textureMipBytes.push_back(std::move(mips));
                inputs.textureResolved.emplace_back();
                inputs.vertexTextureFetch = true;
            }
        }
        // The fetch patch reads stream 0's stride from the UP argument slot;
        // the lowerer's r9 replaces G+0x30F0 until it restores it (0x826A32E4).
        inputs.arguments      = { r[0], r[1], 0, 0, r[4], 0, inputs.indexFormat ? 4U : 0U, 0, particle ? r[6] : state[0x30F0] * 4U, submission.caller };
        inputs.inputsComplete = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        // A refused read keeps the state it read.
        try
        {
            keepState();
        }
        catch (const std::exception&)
        {
        }
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
