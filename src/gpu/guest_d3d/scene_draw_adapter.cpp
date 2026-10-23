#include "scene_draw_adapter.h"

#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>

#include <rex/hash.h>

namespace rerevved::gpu::guest_d3d
{

namespace
{

// The vertex-stage texture fetch slot the shared translator header binds.
constexpr std::uint32_t kVertexTextureSlot = 16;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("scene_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

std::vector<std::uint32_t> microcode(std::span<const std::uint8_t> bytes)
{
    require(!bytes.empty() && bytes.size() % 4 == 0, "scene_microcode_extent");
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        words.push_back(word(bytes, i));
    return words;
}

// Every literal register inside the stage's 256 float4 constants.
bool inStage(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers, bool pixel)
{
    const auto first = kShaderLiteralRegisterBase + (pixel ? kShaderLiteralPixelFirst * 4 : 0);
    return std::ranges::all_of(registers, [&](const auto& entry)
                               {
                                   return entry.first >= first && entry.first < first + 1024;
                               });
}

bool sceneCaller(std::uint64_t caller)
{
    return std::ranges::find(kSceneDrawCallers, caller) != kSceneDrawCallers.end();
}

// k_32_32_FLOAT (37) and k_32_32_32_FLOAT (57) as signed floats, or k_8_8_8_8
// (6) as unsigned bytes, normalized or integer (such as blend indices), from
// fetch 95 inside `stride`.
bool sceneAttribute(const NativeGameplayVertexAttribute& attribute, std::uint32_t stride)
{
    const bool          bytes = attribute.format == 6;
    const std::uint32_t size  = attribute.format == 57 ? 12U : (attribute.format == 37 ? 8U : (bytes ? 4U : 0U));
    return size && attribute.fetchConstant == 95 && attribute.strideBytes == stride && attribute.offsetBytes >= 0 && attribute.offsetBytes % 4 == 0 &&
           std::uint32_t(attribute.offsetBytes) + size <= stride && attribute.isSigned == !bytes && (bytes || !attribute.normalized);
}

// The checks every scene draw makes before its shaders are matched: complete
// inputs, the draw tuple, unconditional unrecorded shaders, stream 0 alone
// and literal records inside their stages.
void requireSceneDraw(const IndexedBufferDrawInputs& inputs, std::size_t textures)
{
    require(inputs.inputsComplete && inputs.textureFetches.size() == textures && inputs.textureBaseBytes.size() == textures && inputs.textureMipBytes.size() == textures,
            "scene_inputs_incomplete");
    // An indexed triangle list or strip through 16-bit big-endian or 32-bit
    // indices at base vertex 0 (VGT_INDX_OFFSET is pinned below); a particle
    // batch draws only triangle lists.
    const auto width  = inputs.indexFormat ? 4U : 2U;
    const bool caller = inputs.arguments.caller == kParticleDrawCaller ? inputs.primitive == 4 : sceneCaller(inputs.arguments.caller);
    require(caller && (inputs.primitive == 4 || inputs.primitive == 6) && !inputs.baseVertex && inputs.indexCount &&
                (inputs.primitive == 6 || inputs.indexCount % 3 == 0) && inputs.indexFormat <= 1 && inputs.indexEndian == (inputs.indexFormat ? 2U : 1U) &&
                indexBytes(inputs).size() == std::uint64_t(inputs.indexCount) * width,
            "scene_draw_tuple");
    require(inputs.state.size() == 0x3500, "scene_conditional_or_recorded_shader");
    // G+0x2ABC bit 6 brackets RenderEntitiesList's Z prepass (0x8269D7D8 to
    // 0x8269DB20): 0x826ADD88 replays each draw depth-only in the Z bins, then
    // normally in the color bins, so a draw passing its own depth equals one.
    if (inputs.state[0x2ABC] & 0x40)
    {
        const auto depth = word(inputs.state, 0x2934), blend = word(inputs.state, 0x2938), func = (depth >> 4) & 7;
        // G+0x2ABE bit 6 selects an alternate depth-only VS for the Z bins.
        // The flush sets it from the bound VS's header word +872 bit 0x20
        // after entry, so a newly bound VS can carry it before the flag does.
        require(!(inputs.state[0x2ABE] & 0x40) && !(word(inputs.vertexHeader, 872) & 0x20), "scene_conditional_or_recorded_shader: zpass depth-only variant");
        if ((depth & 7) != 6 || !(func == 3 || func == 7))
            throw std::runtime_error(std::format("scene_conditional_or_recorded_shader: zpass depth {:#x}", depth));
        if (blend != 0x00010001)
            throw std::runtime_error(std::format("scene_conditional_or_recorded_shader: zpass blend {:#x}", blend));
    }
    else
        require(!(inputs.state[0x2ABE] & 0x40), "scene_conditional_or_recorded_shader");
    require(inputs.streams == std::vector<std::uint32_t>{ 0 } && inputs.streamFetches.size() == 1 && inputs.streamBytes.size() == 1, "scene_stream_form");
    require(inStage(inputs.vertexLiteralRegisters, false) && inStage(inputs.pixelLiteralRegisters, true), "scene_literal_stage");
}

// The leader sampler's refusal codes under the scene prefix.
std::string sceneCode(const std::string& code)
{
    constexpr std::string_view leader = "leader_";
    return code.starts_with(leader) ? "scene_" + code.substr(leader.size()) : code;
}

} // namespace

std::vector<const SceneDrawShader*> SceneDrawCatalog::WithPixel(std::string_view pixelWordDigest) const
{
    std::vector<const SceneDrawShader*> found;
    for (const auto& entry : entries)
        if (entry.shader.pixelGuestWordDigest == pixelWordDigest)
            found.push_back(&entry);
    return found;
}

std::vector<IndexedBufferPixelShader> ScenePixelShaders(const SceneDrawCatalog& catalog)
{
    std::vector<IndexedBufferPixelShader> shaders;
    for (const auto& entry : catalog.entries)
    {
        // The reader snapshots fetch 16 when any pair of the PS samples it.
        const auto found = std::ranges::find(shaders, std::string_view(entry.shader.pixelGuestWordDigest), &IndexedBufferPixelShader::wordDigest);
        if (found == shaders.end())
            shaders.push_back({ entry.shader.pixelGuestWordDigest, entry.pixelTextures, entry.vertexTexture });
        else
            found->vertexTexture |= entry.vertexTexture;
    }
    return shaders;
}

const SceneTargetForm* FindSceneTargetForm(std::span<const std::uint32_t> registers)
{
    const auto found = std::ranges::find_if(kSceneTargetForms, [&](const SceneTargetForm& form)
                                            {
                                                return registers[0x2000] == form.surfaceInfo && registers[0x2001] == form.colorInfo &&
                                                       registers[0x2002] == form.depthInfo && registers[0x2082] == form.scissor;
                                            });
    return found == kSceneTargetForms.end() ? nullptr : &*found;
}

namespace
{

// The presented forms draw the logical 1280x720 surface; an off-screen form
// draws its own extent at its own sample count.
NativeGameplayDrawTarget SceneDrawTarget(const SceneTargetForm& form)
{
    NativeGameplayDrawTarget target;
    target.width  = form.width;
    target.height = form.height;
    // The host holds k_2_10_10_10 as Rgb10a2 and k_16_16 as Rg16Snorm, as
    // the frame assembler decodes RB_COLOR_INFO.
    const auto format = SceneTargetColorFormat(form);
    target.format     = format == 2 ? NativeDrawReplayTargetFormat::Rgb10a2 : format == 4 ? NativeDrawReplayTargetFormat::Rg16Snorm
                                                                                          : NativeDrawReplayTargetFormat::Rgba8;
    if (form.sampleCount == 4 && form.width == 1280 && form.height == 720)
        target.fullViewportTarget = true;
    else
    {
        target.sampleCount = form.sampleCount;
        target.sampleMask  = form.sampleCount == 4 ? 9 : 1;
    }
    return target;
}

} // namespace

bool LoadSceneDrawCatalog(const std::filesystem::path& directory, SceneDrawCatalog& catalog, std::string& error)
{
    catalog = {};
    error.clear();
    std::vector<NativeGameplayShaderInterface> shaders, depthOnly;
    if (!LoadNativeGameplayShaders(directory, shaders, error) || !AdmitSceneDrawShaders(std::move(shaders), catalog, error))
        return false;
    if (!LoadNativeDepthOnlyShaders(directory, depthOnly, error))
    {
        catalog = {};
        return false;
    }
    for (auto& shader : depthOnly)
        catalog.depthOnly.push_back({ std::move(shader) });
    return true;
}

std::vector<std::string_view> SceneDepthOnlyVertexShaders(const SceneDrawCatalog& catalog)
{
    std::vector<std::string_view> shaders;
    for (const auto& entry : catalog.depthOnly)
        if (std::ranges::find(shaders, std::string_view(entry.shader.vertexGuestWordDigest)) == shaders.end())
            shaders.push_back(entry.shader.vertexGuestWordDigest);
    return shaders;
}

bool AdmitSceneDrawShaders(std::vector<NativeGameplayShaderInterface> shaders, SceneDrawCatalog& catalog, std::string& error)
{
    catalog = {};
    error.clear();
    try
    {
        require(!shaders.empty(), "scene_catalog_empty");
        for (auto& shader : shaders)
        {
            SceneDrawShader entry;
            // Pixel slots 0..n-1, then at most the vertex-stage slot.
            const auto& slots   = shader.textureFetchSlots;
            entry.vertexTexture = !slots.empty() && slots.back() == kVertexTextureSlot;
            entry.pixelTextures = static_cast<std::uint32_t>(slots.size() - (entry.vertexTexture ? 1 : 0));
            for (std::uint32_t i = 0; i < entry.pixelTextures; ++i)
                require(slots[i] == i, "scene_catalog_textures");
            for (const auto& other : catalog.entries)
                require(other.shader.pixelGuestWordDigest != shader.pixelGuestWordDigest || other.pixelTextures == entry.pixelTextures, "scene_catalog_textures");
            const auto& attributes = shader.vertexAttributes;
            require(!attributes.empty(), "scene_catalog_layout");
            entry.stride = attributes[0].strideBytes;
            require(entry.stride && entry.stride % 4 == 0 && entry.stride / 4 <= 0xFF, "scene_catalog_layout");
            for (const auto& attribute : attributes)
            {
                require(sceneAttribute(attribute, entry.stride), "scene_catalog_layout");
                entry.attributeOffsets.push_back(std::uint32_t(attribute.offsetBytes));
            }
            entry.shader = std::move(shader);
            catalog.entries.push_back(std::move(entry));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        catalog = {};
        error   = exception.what();
        return false;
    }
}

namespace
{

// Values derived from exact bytes, found by comparing them.
template <typename Value>
class BytesMemo
{
public:
    // Distinct shaders are few; a runaway producer clears the memo instead of
    // growing it.
    static constexpr std::size_t kMaxEntries = 1024;

    const Value* Find(std::span<const std::uint8_t> bytes) const
    {
        const auto [first, last] = index.equal_range(XXH3_64bits(bytes.data(), bytes.size()));
        for (auto found = first; found != last; ++found)
            if (const auto& entry = entries[found->second]; std::ranges::equal(entry.bytes, bytes))
                return &entry.value;
        return nullptr;
    }

    // The value stays in place until the next Insert.
    const Value& Insert(std::span<const std::uint8_t> bytes, Value value)
    {
        if (entries.size() >= kMaxEntries)
        {
            entries.clear();
            index.clear();
        }
        index.emplace(XXH3_64bits(bytes.data(), bytes.size()), entries.size());
        entries.push_back({ { bytes.begin(), bytes.end() }, std::move(value) });
        return entries.back().value;
    }

private:
    struct Entry
    {
        std::vector<std::uint8_t> bytes;
        Value                     value;
    };

    // Entries by the XXH3 of their bytes, as positions in `entries`.
    std::deque<Entry>                                   entries;
    std::unordered_multimap<std::uint64_t, std::size_t> index;
};

// Values derived from exact byte sequences, found by comparing the sequences
// piece by piece, without joining them. The hash reads each piece's size and
// at most kHashedBytes of its head, so only the comparison reads whole pieces.
template <typename Value>
class PiecesMemo
{
public:
    static constexpr std::size_t kMaxEntries  = 1024;
    static constexpr std::size_t kMaxPieces   = 16;
    static constexpr std::size_t kHashedBytes = 32;
    using Pieces                              = std::span<const std::span<const std::uint8_t>>;

    const Value* Find(Pieces pieces) const
    {
        const auto [first, last] = index.equal_range(hash(pieces));
        for (auto found = first; found != last; ++found)
            if (const auto& entry = entries[found->second]; same(entry.bytes, pieces))
                return &entry.value;
        return nullptr;
    }

    // The value stays in place until the next Insert.
    const Value& Insert(Pieces pieces, Value value)
    {
        if (entries.size() >= kMaxEntries)
        {
            entries.clear();
            index.clear();
        }
        std::vector<std::uint8_t> bytes;
        for (const auto piece : pieces)
        {
            const auto size = static_cast<std::uint32_t>(piece.size());
            bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(&size), reinterpret_cast<const std::uint8_t*>(&size) + sizeof(size));
            bytes.insert(bytes.end(), piece.begin(), piece.end());
        }
        index.emplace(hash(pieces), entries.size());
        entries.push_back({ std::move(bytes), std::move(value) });
        return entries.back().value;
    }

private:
    struct Entry
    {
        // Each piece as its 32-bit size, then its bytes.
        std::vector<std::uint8_t> bytes;
        Value                     value;
    };

    static std::uint64_t hash(Pieces pieces)
    {
        if (pieces.size() > kMaxPieces)
            throw std::runtime_error("scene_memo_pieces");
        std::array<std::uint8_t, kMaxPieces * (sizeof(std::uint32_t) + kHashedBytes)> heads;
        std::size_t                                                                   used = 0;
        for (const auto piece : pieces)
        {
            const auto size = static_cast<std::uint32_t>(piece.size());
            const auto head = std::min<std::size_t>(piece.size(), kHashedBytes);
            std::memcpy(heads.data() + used, &size, sizeof(size));
            if (head)
                std::memcpy(heads.data() + used + sizeof(size), piece.data(), head);
            used += sizeof(size) + head;
        }
        return XXH3_64bits(heads.data(), used);
    }

    static bool same(std::span<const std::uint8_t> bytes, Pieces pieces)
    {
        std::size_t offset = 0;
        for (const auto piece : pieces)
        {
            const auto size = static_cast<std::uint32_t>(piece.size());
            if (bytes.size() - offset < sizeof(size) + piece.size() || std::memcmp(bytes.data() + offset, &size, sizeof(size)) != 0 ||
                (!piece.empty() && std::memcmp(bytes.data() + offset + sizeof(size), piece.data(), piece.size()) != 0))
                return false;
            offset += sizeof(size) + piece.size();
        }
        return offset == bytes.size();
    }

    // Entries by the hash of their pieces, as positions in `entries`.
    std::deque<Entry>                                   entries;
    std::unordered_multimap<std::uint64_t, std::size_t> index;
};

// A color draw's selection: the pair its patched VS words select, and the
// words.
struct SceneSelection
{
    std::vector<const SceneDrawShader*> pair;
    std::vector<std::uint8_t>           patched;
};

// A patched depth-only VS: the depth-only entries with its words, its
// decoded fetches as DecodeShaderVertexFetches leaves them, and its words.
struct DepthOnlyCode
{
    std::vector<const SceneDrawShader*>        pair;
    bool                                       decoded = false;
    std::vector<NativeGameplayVertexAttribute> attributes;
    std::vector<std::uint32_t>                 words;
};

// Memo validations hit by digests: the held catalog and the specializations
// made from it stay unchanged, and a draw's microcode words are those its
// entry was selected by, through their digests.
constexpr auto kMemoShaderHit = NativeGameplayShaderValidationCache::Hit::Digests;

// A depth-only entry's interface for one draw's decoded attributes and
// target samples.
struct DepthOnlySpecialization
{
    NativeGameplayShaderInterface       shader;
    NativeGameplayShaderValidationCache validation{ kMemoShaderHit };
};

struct SceneEntryMemo
{
    // Patched VS bytes whose word digest is the entry's.
    std::vector<std::uint8_t> digestVertexCode;
    // The patched VS and PS bytes the words and fetch layout derive from.
    std::vector<std::uint8_t>           vertexCode, pixelCode;
    std::vector<std::uint32_t>          vertexWords, pixelWords;
    ShaderFetchMatch                    layout = ShaderFetchMatch::Matched;
    NativeGameplayShaderValidationCache validation{ kMemoShaderHit };
    std::deque<DepthOnlySpecialization> depthOnly;
};

// Geometry an earlier build decoded: the stream's retained buffer, which
// the entry holds so that no other buffer takes its address, with the fetch,
// attributes, index controls and index bytes it decoded from.
struct SceneGeometry
{
    std::shared_ptr<const std::vector<std::uint8_t>> stream;
    std::uint32_t                                    fetchConstant = 0, base = 0;
    std::vector<NativeGameplayVertexAttribute>       attributes;
    std::array<std::uint32_t, 10>                    controls{};
    GuestBytes                                       indexBytes;
    std::shared_ptr<const NativeDrawReplayIndices>   indices;
    std::shared_ptr<const std::vector<std::uint8_t>> vertices;
    std::size_t                                      bytes = 0;
    bool                                             used  = true;
};

// What the index conversion reads besides the index bytes.
std::array<std::uint32_t, 10> indexControls(const NativeGameplayDrawView& view)
{
    return { view.primitive, view.indexed, view.indexCount, view.indexFormat, view.indexEndian, view.registers[0x2100], view.registers[0x2101], view.registers[0x2102], view.registers[0x2103], view.registers[0x2205] };
}

bool sameAttributes(std::span<const NativeGameplayVertexAttribute> a, std::span<const NativeGameplayVertexAttribute> b)
{
    return std::ranges::equal(a, b, [](const NativeGameplayVertexAttribute& x, const NativeGameplayVertexAttribute& y)
                              {
                                  return x.fetchConstant == y.fetchConstant && x.strideBytes == y.strideBytes && x.offsetBytes == y.offsetBytes &&
                                         x.format == y.format && x.isSigned == y.isSigned && x.normalized == y.normalized;
                              });
}

} // namespace

struct SceneBuildCacheState
{
    SceneBuildCacheState(std::shared_ptr<const SceneDrawCatalog> served, std::size_t geometryBudgetBytes)
    : held(std::move(served))
    , catalog(held.get())
    , geometryBudget(geometryBudgetBytes)
    {
        if (catalog)
            entries.resize(catalog->entries.size() + catalog->depthOnly.size());
    }

    // The memo's entry pointers are into this catalog, held so that no other
    // catalog takes its address.
    std::shared_ptr<const SceneDrawCatalog>        held;
    const SceneDrawCatalog*                        catalog = nullptr;
    BytesMemo<std::vector<const SceneDrawShader*>> candidates;
    // By the candidates and every input of the fold's words and the fetch patch.
    PiecesMemo<SceneSelection> selections;
    BytesMemo<DepthOnlyCode>   depthOnlyCode;
    // Per catalog entry, then per depth-only entry, by position.
    std::vector<SceneEntryMemo> entries;
    // Decoded geometry by a digest of its key; entries of a stream refreshed
    // since are found no more and go at a sweep.
    std::unordered_multimap<std::uint64_t, SceneGeometry> geometry;
    std::size_t                                           geometryBudget = 0, geometryBytes = 0, builds = 0;
    // GuestDrawRegisters' words: zero but for those it emits and those the
    // previous build wrote, listed in `written`.
    std::vector<std::uint32_t> registers = std::vector<std::uint32_t>(kGuestDrawRegisterCount);
    std::vector<std::uint32_t> written;

    // Whether the memo serves a build against `of`.
    bool Begin(const SceneDrawCatalog& of)
    {
        if (catalog != &of || entries.size() != of.entries.size() + of.depthOnly.size())
            return false;
        if (++builds % SceneBuildCache::kSweepBuilds == 0)
            for (auto it = geometry.begin(); it != geometry.end();)
                if (!it->second.used)
                {
                    geometryBytes -= it->second.bytes;
                    it = geometry.erase(it);
                }
                else
                    (it++)->second.used = false;
        return true;
    }

    static std::uint64_t GeometryKey(const NativeGameplayDrawView& view, std::span<const NativeGameplayVertexAttribute> attributes)
    {
        const auto& fetch = view.vertexFetches[0];
        // Index bytes stay out of the key: a hit compares them once.
        std::array<std::uint64_t, 14 + 16 * 5> words{ reinterpret_cast<std::uintptr_t>(fetch.bytes.data()), fetch.fetchConstant, fetch.guestBase, attributes.size() };
        std::size_t                            count = 4;
        for (const auto value : indexControls(view))
            words[count++] = value;
        for (const auto& attribute : attributes.first(std::min<std::size_t>(attributes.size(), 16)))
            for (const std::uint64_t value : { std::uint64_t(attribute.fetchConstant), std::uint64_t(attribute.strideBytes), std::uint64_t(std::uint32_t(attribute.offsetBytes)), std::uint64_t(attribute.format), std::uint64_t(attribute.isSigned) << 1 | attribute.normalized })
                words[count++] = value;
        return XXH3_64bits(words.data(), count * sizeof(words[0]));
    }

    // The geometry decoded from `stream`'s retained buffer, as one fetch,
    // with these attributes, index controls and index bytes.
    SceneGeometry* FindGeometry(const GuestBytes& stream, const GuestBytes& indexSource, const NativeGameplayDrawView& view, std::span<const NativeGameplayVertexAttribute> attributes)
    {
        if (!stream.IsShared() || view.vertexFetches.size() != 1 || view.vertexFetches[0].bytes.data() != stream.data() || view.vertexFetches[0].bytes.size() != stream.size())
            return nullptr;
        const auto& fetch    = view.vertexFetches[0];
        const auto  controls = indexControls(view);
        for (auto [it, end] = geometry.equal_range(GeometryKey(view, attributes)); it != end; ++it)
        {
            auto& entry = it->second;
            if (entry.stream == stream.Buffer() && entry.fetchConstant == fetch.fetchConstant && entry.base == fetch.guestBase && entry.controls == controls &&
                sameAttributes(entry.attributes, attributes) &&
                ((entry.indexBytes.SharesBuffer(indexSource) && entry.indexBytes.data() == view.indexBytes.data() && entry.indexBytes.size() == view.indexBytes.size()) ||
                 std::ranges::equal(entry.indexBytes, view.indexBytes)))
            {
                // Move-assign so a formerly owned copy releases its storage.
                if (indexSource.IsShared() && indexSource.data() == view.indexBytes.data() && indexSource.size() == view.indexBytes.size())
                    entry.indexBytes = GuestBytes::Shared(indexSource.Buffer());
                entry.used = true;
                return &entry;
            }
        }
        return nullptr;
    }

    // Keeps a new build's geometry within the budget, its recipe sharing
    // the decoded vertices and indices.
    void RememberGeometry(const GuestBytes& stream, const GuestBytes& indexSource, const NativeGameplayDrawView& view, std::span<const NativeGameplayVertexAttribute> attributes, NativeDrawReplayRecipe& native)
    {
        if (!stream.IsShared() || view.vertexFetches.size() != 1 || view.vertexFetches[0].bytes.data() != stream.data() || view.vertexFetches[0].bytes.size() != stream.size() ||
            native.sharedVertexData || native.vertexData.empty() || !native.vertexStrideBytes)
            return;
        // Keep the index storage charge independent of its ownership representation.
        const auto bytes = native.vertexData.size() + native.indexSpan().size() * sizeof(std::uint32_t) + view.indexBytes.size() + stream.size() + sizeof(SceneGeometry) - sizeof(GuestBytes) + sizeof(std::vector<std::uint8_t>);
        if (bytes > geometryBudget - std::min(geometryBudget, geometryBytes))
            return;
        SceneGeometry entry;
        entry.stream        = stream.Buffer();
        entry.fetchConstant = view.vertexFetches[0].fetchConstant;
        entry.base          = view.vertexFetches[0].guestBase;
        entry.attributes.assign(attributes.begin(), attributes.end());
        entry.controls = indexControls(view);
        if (indexSource.IsShared() && indexSource.data() == view.indexBytes.data() && indexSource.size() == view.indexBytes.size())
            entry.indexBytes = indexSource;
        else
            entry.indexBytes = std::vector<std::uint8_t>(view.indexBytes.begin(), view.indexBytes.end());
        if (!native.sharedIndices)
        {
            // A failed allocation here leaves the recipe its own indices.
            native.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::move(native.indices));
            native.indices       = {};
        }
        entry.indices           = native.sharedIndices;
        entry.bytes             = bytes;
        entry.vertices          = std::make_shared<const std::vector<std::uint8_t>>(std::move(native.vertexData));
        native.vertexData       = {};
        native.sharedVertexData = entry.vertices;
        geometryBytes += bytes;
        geometry.emplace(GeometryKey(view, attributes), std::move(entry));
    }

    // Builds `native` from `view`, with the remembered geometry of a draw
    // whose stream is `stream`.
    bool BuildNative(NativeGameplayDrawView view, const GuestBytes& stream, const GuestBytes& indexSource, const NativeGameplayShaderInterface& shader, const NativeGameplayDrawTarget& target, NativeDrawReplayRecipe& native, std::string& error, NativeGameplayShaderValidationCache* validation)
    {
        NativeGameplayDecodedGeometry prepared;
        const auto*                   known = FindGeometry(stream, indexSource, view, shader.vertexAttributes);
        if (known)
        {
            prepared             = { known->indices, known->vertices };
            view.decodedGeometry = &prepared;
        }
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, native, error, validation))
            return false;
        if (!known)
            RememberGeometry(stream, indexSource, view, shader.vertexAttributes, native);
        return true;
    }

    // The memo of `entry`, which is in the catalog's entries or depth-only list.
    SceneEntryMemo& Entry(const SceneDrawShader* entry)
    {
        if (!catalog->entries.empty() && entry >= catalog->entries.data() && entry < catalog->entries.data() + catalog->entries.size())
            return entries[std::size_t(entry - catalog->entries.data())];
        if (!catalog->depthOnly.empty() && entry >= catalog->depthOnly.data() && entry < catalog->depthOnly.data() + catalog->depthOnly.size())
            return entries[catalog->entries.size() + std::size_t(entry - catalog->depthOnly.data())];
        throw std::runtime_error("scene_build_cache_entry");
    }

    std::span<std::uint32_t> Registers(std::span<const std::uint8_t> state)
    {
        for (const auto index : written)
            registers[index] = 0;
        written.clear();
        FillGuestDrawRegisters(state, registers);
        return registers;
    }
};

struct SceneBuildCacheAccess
{
    static std::unique_lock<std::mutex> Lock(SceneBuildCache& cache)
    {
        return std::unique_lock(cache.mutex);
    }

    static SceneBuildCacheState& State(SceneBuildCache& cache)
    {
        return *cache.state;
    }
};

SceneBuildCache::SceneBuildCache(std::shared_ptr<const SceneDrawCatalog> catalog, std::size_t geometryBudgetBytes)
: state(std::make_unique<SceneBuildCacheState>(std::move(catalog), geometryBudgetBytes))
{
}

SceneBuildCache::~SceneBuildCache() = default;

std::size_t SceneBuildCache::GeometryBytes()
{
    std::lock_guard lock(mutex);
    return state->geometryBytes;
}

namespace
{

// A build's register file: the memo's, or its own words.
struct SceneRegisters
{
    std::vector<std::uint32_t> owned;
    std::span<std::uint32_t>   words;
    SceneBuildCacheState*      memo = nullptr;

    SceneRegisters(std::span<const std::uint8_t> state, SceneBuildCacheState* memo)
    : memo(memo)
    {
        if (memo)
            words = memo->Registers(state);
        else
        {
            owned = GuestDrawRegisters(state);
            words = owned;
        }
    }

    void Set(std::uint32_t index, std::uint32_t value)
    {
        words[index] = value;
        if (memo)
            memo->written.push_back(index);
    }
};

// The memo of a build, held for the build when there is a cache.
struct SceneBuildMemo
{
    std::unique_lock<std::mutex> lock;
    SceneBuildCacheState*        state = nullptr;

    SceneBuildMemo(SceneBuildCache* build, const SceneDrawCatalog& catalog)
    {
        if (!build)
            return;
        lock  = SceneBuildCacheAccess::Lock(*build);
        state = &SceneBuildCacheAccess::State(*build);
        if (!state->Begin(catalog))
        {
            state = nullptr;
            lock.unlock();
        }
    }
};

bool depthOnlyNative(const NativeGameplayDrawView& view, std::span<const std::uint8_t> effectiveState, const SceneDrawShader& entry, const SceneTargetForm& form, std::span<const NativeGameplayVertexAttribute> attributes, NativeDrawReplayRecipe& native, std::string& error, SceneBuildCacheState* memo, const GuestBytes* stream, const GuestBytes* indexSource);
bool buildSceneDepthOnlyRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, SceneBuildCache* build);

// A color build's working buffers, kept per thread so later builds reuse
// their capacity; each build overwrites all it reads.
struct SceneBuildScratch
{
    std::vector<std::uint8_t>                 folded;
    std::vector<NativeGameplayTextureBinding> bindings;
};

// BuildSceneDrawRecipe into an empty recipe and error.
bool buildSceneDrawRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, LeaderTextureCache* cache, SceneBuildCache* build)
{
    if (!inputs.pixelObject)
        return buildSceneDepthOnlyRecipe(inputs, catalog, recipe, error, build);
    if (inputs.pixelCode.empty() || inputs.pixelCode.size() % 4)
        return false;
    const SceneBuildMemo                       memo(build, catalog);
    const std::vector<const SceneDrawShader*>* known = memo.state ? memo.state->candidates.Find(inputs.pixelCode) : nullptr;
    std::vector<const SceneDrawShader*>        computed;
    if (!known)
    {
        computed = catalog.WithPixel(IndexedBufferMicrocodeWordDigest(inputs.pixelCode));
        if (memo.state)
            known = &memo.state->candidates.Insert(inputs.pixelCode, computed);
    }
    const auto& candidates = known ? *known : computed;
    if (candidates.empty())
        return false;
    try
    {
        const auto pixelTextures = candidates.front()->pixelTextures;
        requireSceneDraw(inputs, pixelTextures + (inputs.vertexTextureFetch ? 1 : 0));

        thread_local SceneBuildScratch scratch;
        // 0x826ADD88 applies linkage before the fetch patch, whose admitted
        // offsets are each candidate's own; the patched words select the pair.
        auto& folded = scratch.folded;
        folded.assign(inputs.vertexCode.begin(), inputs.vertexCode.end());
        if (!FoldSolidShaderMetadata(inputs, folded, recipe.effectiveState, error))
            return false;
        auto& pair = recipe.pair;
        // Fetch patch reads 16 bytes at G+0x30F0; passed fold leaves
        // 0x3500-byte shadow.
        const std::array<std::uint32_t, 2>                  scalars{ inputs.arguments.stride, inputs.variant };
        const std::array<std::span<const std::uint8_t>, 11> selectionKey{
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(candidates.data()), candidates.size() * sizeof(candidates[0])),
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(scalars.data()), sizeof(scalars)),
            inputs.vertexCode,
            inputs.vertexMetadata,
            inputs.pixelMetadata,
            inputs.declaration,
            inputs.fetchTemplates,
            inputs.fetchRecords,
            inputs.swizzleTable,
            inputs.formatTable,
            std::span<const std::uint8_t>(inputs.state).subspan(0x30F0, 16),
        };
        const SceneSelection* selected = memo.state ? memo.state->selections.Find(selectionKey) : nullptr;
        if (selected)
        {
            pair                     = selected->pair;
            recipe.patchedVertexCode = selected->patched;
        }
        // Candidates admitting the same offsets patch alike.
        const std::vector<std::uint32_t>* patchedOffsets = nullptr;
        bool                              patchedOk      = false;
        std::vector<std::uint8_t>         patchedCode;
        std::string                       patchedDigest;
        // Whether the patched words are the candidate's, by its digest.
        const auto candidateWords = [&](const SceneDrawShader* candidate)
        {
            SceneEntryMemo* entryMemo = memo.state ? &memo.state->Entry(candidate) : nullptr;
            if (entryMemo && !entryMemo->digestVertexCode.empty() && entryMemo->digestVertexCode == patchedCode)
                return true;
            if (patchedDigest.empty())
                patchedDigest = IndexedBufferMicrocodeWordDigest(patchedCode);
            if (patchedDigest != candidate->shader.vertexGuestWordDigest)
                return false;
            if (entryMemo)
                entryMemo->digestVertexCode = patchedCode;
            return true;
        };
        for (const auto* candidate : selected ? std::span<const SceneDrawShader* const>() : std::span<const SceneDrawShader* const>(candidates))
        {
            if (!pair.empty())
            {
                if (candidate->shader.vertexGuestWordDigest == pair.front()->shader.vertexGuestWordDigest)
                    pair.push_back(candidate);
                continue;
            }
            if (word(inputs.vertexMetadata, 28) != candidate->attributeOffsets.size() || inputs.arguments.stride != candidate->stride)
                continue;
            if (!patchedOffsets || *patchedOffsets != candidate->attributeOffsets)
            {
                patchedOffsets      = &candidate->attributeOffsets;
                const auto identity = MemoizedVertexShaderIdentity(inputs, candidate->attributeOffsets, true);
                patchedOk           = identity->ok;
                patchedCode         = identity->code;
                patchedDigest       = identity->digest;
            }
            if (!patchedOk || !candidateWords(candidate))
                continue;
            recipe.patchedVertexCode = patchedCode;
            pair.push_back(candidate);
        }
        if (pair.empty())
        {
            error = "scene_catalog_vertex: " + IndexedBufferMicrocodeWordDigest(inputs.vertexCode).substr(0, 16);
            return false;
        }
        if (memo.state && !selected)
            memo.state->selections.Insert(selectionKey, { pair, recipe.patchedVertexCode });

        SceneRegisters guestRegisters(recipe.effectiveState, memo.state);
        const auto     registers = guestRegisters.words;
        recipe.form              = FindSceneTargetForm(registers);
        if (!recipe.form)
        {
            error = std::format("scene_target_form: {:#x} {:#x} {:#x} {:#x}", registers[0x2000], registers[0x2001], registers[0x2002], registers[0x2082]);
            return false;
        }
        // The pair's VS samples fetch 16, which the reader snapshots after
        // the pixel slots for every PS paired with such a VS.
        const bool vertexTexture = pair.front()->vertexTexture;
        require(!vertexTexture || inputs.vertexTextureFetch, "scene_vertex_fetch_unread");
        // RB_COLORCONTROL: no alpha-to-mask offsets. ALPHA_FUNC (bits 0..2) has
        // no effect while ALPHA_TEST_ENABLE (bit 3) is clear; an enabled test
        // has Greater (4) and GreaterEqual (6) specializations.
        const auto colorControl = registers[0x2202];
        if ((colorControl & ~0xFU) != 0x87000000U)
        {
            error = std::format("scene_color_control: {:#x}", colorControl);
            return false;
        }
        const bool alphaTest = (colorControl & 8) != 0;
        const auto alphaFunc = colorControl & 7;
        if (alphaTest && alphaFunc != 4 && alphaFunc != 6)
        {
            error = std::format("scene_alpha_func: {:#x}", colorControl);
            return false;
        }
        // PA_CL_CLIP_CNTL 0x80001 (UCP_ENA_0) takes the entry whose VS clips by
        // user plane 0; the hall reflections at 0x8250C768 draw with it.
        const bool userClip = registers[0x2204] == kSceneUserClipControl;
        const auto chosen   = std::ranges::find_if(pair, [&](const SceneDrawShader* entry)
                                                   {
                                                     const auto& shader = entry->shader;
                                                     return shader.alphaTestEnabled == alphaTest && (!alphaTest || shader.alphaTestCompare == alphaFunc) &&
                                                            shader.sampleCount == recipe.form->sampleCount && shader.colorTargetFormat == SceneTargetColorFormat(*recipe.form) && shader.colorExpBias == 0 &&
                                                            shader.colorTargetCount == 1 && !shader.screenSpacePosition && shader.userClipPlane == userClip && shader.shaderProgram == registers[0x2180] &&
                                                            shader.shaderContext == registers[0x2181] && NativeGameplayInterpolatorsMatch(shader, registers[0x2182]);
                                                   });
        if (chosen == pair.end())
        {
            error = std::format("scene_specialization: alpha {} samples {} program {:#x} context {:#x} interpolators {:#x}{}", alphaTest ? alphaFunc : 0, recipe.form->sampleCount, registers[0x2180], registers[0x2181], registers[0x2182], userClip ? " user clip plane 0" : "");
            return false;
        }
        recipe.entry       = *chosen;
        const auto& shader = recipe.entry->shader;
        // The live microcode decides the layout and bindings; the entry must agree.
        SceneEntryMemo* entryMemo = memo.state ? &memo.state->Entry(recipe.entry) : nullptr;
        if (entryMemo && (entryMemo->vertexWords.empty() || entryMemo->vertexCode != recipe.patchedVertexCode || entryMemo->pixelCode != inputs.pixelCode))
        {
            const auto layout      = MatchShaderFetches(recipe.patchedVertexCode, inputs.pixelCode, shader);
            auto       vertexWords = microcode(recipe.patchedVertexCode), pixelWords = microcode(inputs.pixelCode);
            entryMemo->vertexCode  = recipe.patchedVertexCode;
            entryMemo->pixelCode   = inputs.pixelCode;
            entryMemo->vertexWords = std::move(vertexWords);
            entryMemo->pixelWords  = std::move(pixelWords);
            entryMemo->layout      = layout;
        }
        const auto layout = entryMemo ? entryMemo->layout : MatchShaderFetches(recipe.patchedVertexCode, inputs.pixelCode, shader);
        require(layout != ShaderFetchMatch::VertexLayout, "scene_vertex_fetch_layout");
        require(layout != ShaderFetchMatch::TextureLayout, "scene_texture_fetch_layout");

        // Inside the Z prepass a colorless draw's depth would occlude earlier
        // colored draws in their color bins; one native pass cannot.
        const bool zpass  = inputs.state[0x2ABC] & 0x40;
        const bool packed = SceneTargetColorFormat(*recipe.form) == 4 && std::ranges::find(kScenePackedColorMasks, registers[0x2104]) != kScenePackedColorMasks.end();
        for (const auto [index, value] : kSceneFixedState)
            if (registers[index] != value && !(index == 0x2104 && ((registers[index] == kSceneNoColorMask && !zpass) || packed)) && !(index == 0x2204 && userClip))
            {
                error = std::format("scene_fixed_state {:#x}: {:#x}", index, registers[index]);
                return false;
            }
        for (const auto& [index, forms, code] : { std::tuple{ 0x2200U, std::span<const std::uint32_t>(kSceneDepthControls), "scene_depth_control" },
                                                  std::tuple{ 0x2201U, std::span<const std::uint32_t>(kSceneBlendControls), "scene_blend_control" },
                                                  std::tuple{ 0x2205U, std::span<const std::uint32_t>(kSceneRasterControls), "scene_raster_control" } })
            if (std::ranges::find(forms, registers[index]) == forms.end())
            {
                error = std::format("{}: {:#x}", code, registers[index]);
                return false;
            }
        // The host replays k_2_10_10_10 and k_16_16 targets without a depth
        // or stencil attachment, so their draws must leave both off.
        if (SceneTargetColorFormat(*recipe.form) != 0 && (registers[0x2200] & 7) != 0)
        {
            error = std::format("scene_packed_target_depth: {:#x}", registers[0x2200]);
            return false;
        }
        // The shadow lacks the literal records; 0x826ADB98 loads them in order.
        for (const auto* literals : { &inputs.vertexLiteralRegisters, &inputs.pixelLiteralRegisters })
            for (const auto [index, value] : *literals)
                guestRegisters.Set(index, value);

        // Index i binds pixel slot i, and the last one slot 16 when the VS
        // samples it; the converter names that slot's descriptor last too.
        const auto& dimensions = shader.textureDimensions;
        const auto  textures   = pixelTextures + (vertexTexture ? 1U : 0U);
        recipe.textures.resize(textures);
        recipe.samplers.resize(textures);
        auto& bindings = scratch.bindings;
        bindings.clear();
        for (std::uint32_t slot = 0; slot < textures; ++slot)
        {
            const auto  guestSlot = slot < pixelTextures ? slot : kVertexTextureSlot;
            const auto& fetch     = inputs.textureFetches[slot];
            for (std::size_t i = 0; i < fetch.size(); ++i)
                require(word(inputs.state, 0x480 + guestSlot * 24 + i * 4) == fetch[i], "scene_texture_fetch_mismatch");
            // Fetch dword 1 bits 0..5 hold the texture format.
            const auto format = fetch[1] & 63;
            if (std::ranges::find(kSceneTextureFormats, format) == kSceneTextureFormats.end())
            {
                error = std::format("scene_texture_format {}", format);
                return false;
            }
            // Fetch dword 4 bits 12..21 are signed LOD bias in 1/32 steps, used
            // as host mip bias. Equal mag(dword 3 bits 19..20)/min(21..22)
            // filters keep bias from changing filter choice.
            auto       sampled = fetch;
            const auto bias    = static_cast<std::int32_t>(fetch[4] << 10) >> 22;
            if (bias && ((fetch[3] >> 19) & 3) == ((fetch[3] >> 21) & 3))
                sampled[4] &= ~(0x3FFU << 12);
            if (!DecodeLeaderSampler(sampled, dimensions[slot], recipe.samplers[slot], error, kLeaderAnisotropicOverride))
            {
                error = sceneCode(error);
                return false;
            }
            recipe.samplers[slot].mipBias = float(bias) / 32.0F;
            const auto& base              = inputs.textureBaseBytes[slot];
            const auto& mips              = inputs.textureMipBytes[slot];
            std::string textureError;
            if (slot < inputs.textureResolved.size() && inputs.textureResolved[slot])
                recipe.textures[slot] = BindNativeResolvedTexture(*inputs.textureResolved[slot]);
            else if (!(cache ? cache->Decode(fetch, base, mips, recipe.textures[slot], textureError)
                             : DecodeNativeTextureVersioned(fetch, base, mips, recipe.textures[slot], textureError)))
            {
                error = "scene_texture_decode: " + textureError;
                return false;
            }
            require(recipe.textures[slot].dimension == dimensions[slot], "scene_texture_dimension");
            bindings.push_back({ guestSlot, &recipe.textures[slot], &recipe.samplers[slot] });
        }

        // Vertex fetch 95 is stream 0's guest descriptor and span.
        const auto& stream = inputs.streamFetches[0];
        guestRegisters.Set(0x48BE, stream[0]);
        guestRegisters.Set(0x48BF, stream[1]);
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, stream[0] & ~3U, inputs.streamBytes[0] } } };
        std::vector<std::uint32_t>                         vs, ps;
        if (!entryMemo)
        {
            vs = microcode(recipe.patchedVertexCode);
            ps = microcode(inputs.pixelCode);
        }
        NativeGameplayDrawView view;
        view.registers       = registers;
        view.vertexMicrocode = entryMemo ? entryMemo->vertexWords : vs;
        view.pixelMicrocode  = entryMemo ? entryMemo->pixelWords : ps;
        view.vertexFetches   = fetches;
        view.textureBindings = bindings;
        view.indexBytes      = indexBytes(inputs);
        view.primitive       = inputs.primitive;
        view.indexCount      = inputs.indexCount;
        view.indexFormat     = inputs.indexFormat;
        view.indexEndian     = inputs.indexEndian;
        auto target          = SceneDrawTarget(*recipe.form);
        if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(recipe.effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "scene_viewport_shadow_scale_mismatch");
        if (!(memo.state ? memo.state->BuildNative(view, inputs.streamBytes[0], inputs.indexBytes, shader, target, recipe.native, error, &entryMemo->validation)
                         : BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error)))
        {
            error = "scene_recipe_rejected: " + error;
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace

bool BuildSceneDrawRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, LeaderTextureCache* cache, SceneBuildCache* build)
{
    recipe = {};
    error.clear();
    return buildSceneDrawRecipe(inputs, catalog, recipe, error, cache, build);
}

namespace
{

std::shared_ptr<const VertexShaderIdentity> depthOnlyIdentity(const IndexedBufferDrawInputs& inputs, std::optional<std::uint32_t> strideOverride = {})
{
    std::vector<std::uint32_t> offsets;
    for (std::size_t element = 0; element + 12 <= inputs.declaration.size(); element += 12)
        offsets.push_back(word(inputs.declaration, element) & 0xFFFF);
    return MemoizedVertexShaderIdentity(inputs, offsets, false, strideOverride);
}

} // namespace

bool PatchDepthOnlyVertexFetch(const IndexedBufferDrawInputs& inputs, std::vector<std::uint8_t>& code, std::string& error)
{
    const auto identity = depthOnlyIdentity(inputs);
    code                = identity->code;
    error               = identity->error;
    return identity->ok;
}

bool BuildSceneDepthOnlyRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, SceneBuildCache* build)
{
    recipe = {};
    error.clear();
    return buildSceneDepthOnlyRecipe(inputs, catalog, recipe, error, build);
}

namespace
{

// BuildSceneDepthOnlyRecipe into an empty recipe and error.
bool buildSceneDepthOnlyRecipe(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, SceneDrawRecipe& recipe, std::string& error, SceneBuildCache* build)
{
    if (inputs.pixelObject || inputs.vertexCode.empty() || inputs.vertexCode.size() % 4)
        return false;
    const SceneBuildMemo memo(build, catalog);
    // The list pins the words the GPU runs: those 0x826AD150's fetch patch
    // writes for this draw. The words read at entry are the previous patch's,
    // or before the VS's first draw the shader file's fetch templates.
    const auto listed = [&](const std::string& digest)
    {
        return std::ranges::any_of(catalog.depthOnly, [&](const SceneDrawShader& entry)
                                   {
                                       return entry.shader.vertexGuestWordDigest == digest;
                                   });
    };
    const auto identity       = depthOnlyIdentity(inputs);
    const bool patched        = identity->ok;
    recipe.patchedVertexCode  = identity->code;
    const DepthOnlyCode* code = patched && memo.state ? memo.state->depthOnlyCode.Find(recipe.patchedVertexCode) : nullptr;
    if (code)
        recipe.pair = code->pair;
    else if (patched)
    {
        const auto& digest = identity->digest;
        for (const auto& entry : catalog.depthOnly)
            if (entry.shader.vertexGuestWordDigest == digest)
                recipe.pair.push_back(&entry);
        // A patch keeps the VS's whole 12-byte instructions.
        if (memo.state && !recipe.patchedVertexCode.empty() && recipe.patchedVertexCode.size() % 4 == 0)
        {
            DepthOnlyCode value;
            value.pair    = recipe.pair;
            value.decoded = DecodeShaderVertexFetches(recipe.patchedVertexCode, value.attributes);
            value.words   = microcode(recipe.patchedVertexCode);
            code          = &memo.state->depthOnlyCode.Insert(recipe.patchedVertexCode, std::move(value));
        }
    }
    // A listed VS that this draw's patch rewrites is refused below by name.
    if (recipe.pair.empty() && !listed(IndexedBufferMicrocodeWordDigest(inputs.vertexCode)))
        return false;
    try
    {
        requireSceneDraw(inputs, 0);
        // As a colorless draw, its prepass depth would occlude earlier colored draws.
        require(!(inputs.state[0x2ABC] & 0x40), "scene_conditional_or_recorded_shader: zpass depth-only draw");
        require(inputs.pixelCode.empty() && inputs.pixelLiteralRegisters.empty(), "scene_inputs_incomplete");
        // Variant one has its own metadata, which the fetch patch does not read.
        require(inputs.variant == 0, "scene_depth_only_variant");
        require(patched && !recipe.pair.empty(), "scene_depth_only_fetch_patch");
        // The patched words decide the layout.
        // The memo's attributes stay in place: this build inserts no more.
        std::vector<NativeGameplayVertexAttribute>     decodedAttributes;
        std::span<const NativeGameplayVertexAttribute> attributes;
        bool                                           decoded = false;
        if (code)
        {
            attributes = code->attributes;
            decoded    = code->decoded;
        }
        else
        {
            decoded    = DecodeShaderVertexFetches(recipe.patchedVertexCode, decodedAttributes);
            attributes = decodedAttributes;
        }
        require(decoded && !attributes.empty() && attributes.size() == word(inputs.vertexMetadata, 28), "scene_vertex_fetch_layout");
        for (const auto& attribute : attributes)
            require(sceneAttribute(attribute, inputs.arguments.stride), "scene_vertex_fetch_layout");
        recipe.effectiveState = inputs.state;
        if (!CommitSceneDepthOnlyState(recipe.effectiveState, inputs.vertexMetadata, error))
            return false;
        SceneRegisters guestRegisters(recipe.effectiveState, memo.state);
        const auto     registers = guestRegisters.words;
        if (!CheckSceneDepthOnlyState(registers, recipe.pair, recipe.form, recipe.entry, error))
            return false;
        for (const auto [index, value] : inputs.vertexLiteralRegisters)
            guestRegisters.Set(index, value);

        const auto& stream = inputs.streamFetches[0];
        guestRegisters.Set(0x48BE, stream[0]);
        guestRegisters.Set(0x48BF, stream[1]);
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, stream[0] & ~3U, inputs.streamBytes[0] } } };
        std::vector<std::uint32_t>                         vs;
        if (!code)
            vs = microcode(recipe.patchedVertexCode);
        NativeGameplayDrawView view;
        view.registers       = registers;
        view.vertexMicrocode = code ? code->words : vs;
        view.vertexFetches   = fetches;
        view.indexBytes      = indexBytes(inputs);
        view.primitive       = inputs.primitive;
        view.indexCount      = inputs.indexCount;
        view.indexFormat     = inputs.indexFormat;
        view.indexEndian     = inputs.indexEndian;
        return depthOnlyNative(view, recipe.effectiveState, *recipe.entry, *recipe.form, attributes, recipe.native, error, memo.state, &inputs.streamBytes[0], &inputs.indexBytes);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace

bool CommitSceneDepthOnlyState(std::vector<std::uint8_t>& state, std::span<const std::uint8_t> vertexMetadata, std::string& error)
{
    error.clear();
    try
    {
        require(state.size() == 0x3500 && vertexMetadata.size() >= 32, "scene_inputs_incomplete");
        // 0x826ADA38 would call the linkage routine 0x826AD8E8 for a VS with
        // exports to link, which needs a pixel shader's records.
        require((word(vertexMetadata, 20) & 31) == 0, "scene_depth_only_linkage");
        const auto put = [&](std::size_t offset, std::uint32_t value)
        {
            for (unsigned c = 0; c < 4; ++c)
                state[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
        };
        // The draw callers (0x826A3608 and its siblings) pass the dirty
        // doubleword at G+16 and call 0x826ADD88 only when its low word has
        // a bit of 0x1E0000; otherwise the shadow stays as it is.
        auto dirty = word(state, 20);
        if (!(dirty & 0x1E0000U))
            return true;
        // loc_826ADE2C: no pixel shader sets RB_MODECONTROL (G+0x2954) to 5,
        // marking 0x80008 dirty; with G+0x2ABF bit 3 it also clears
        // RB_COLOR_INFO's format bits.
        const auto mode = word(state, 0x2954);
        if ((mode & 7) != 5)
        {
            put(0x2954, (mode & ~7U) | 5);
            if (state[0x2ABF] & 8)
                put(0x2884, word(state, 0x2884) & ~0xF0000U);
            dirty |= 0x80008U;
        }
        // loc_826AE1F0: with 0x180000 dirty, the VS metadata's program and
        // context words without pixel bits replace the shadow's; otherwise
        // the shadow keeps the last commit's.
        if (dirty & 0x180000U)
        {
            put(0x2920, word(vertexMetadata, 8));
            put(0x2924, word(vertexMetadata, 12));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool CheckSceneDepthOnlyState(std::span<std::uint32_t> registers, std::span<const SceneDrawShader* const> pair, const SceneTargetForm*& form, const SceneDrawShader*& entry, std::string& error)
{
    form  = FindSceneTargetForm(registers);
    entry = nullptr;
    error.clear();
    // A depth-only draw needs the depth attachment a packed form lacks.
    if (!form || SceneTargetColorFormat(*form) != 0)
    {
        form  = nullptr;
        error = std::format("scene_target_form: {:#x} {:#x} {:#x} {:#x}", registers[0x2000], registers[0x2001], registers[0x2002], registers[0x2082]);
        return false;
    }
    // No alpha-to-mask or offsets. An alpha test is a no-op here: the SDK
    // binds no PS in RB_MODECONTROL 5, and only a translated PS alpha tests.
    if ((registers[0x2202] & ~0xFU) != 0x87000000U)
    {
        error = std::format("scene_color_control: {:#x}", registers[0x2202]);
        return false;
    }
    for (const auto [index, value] : kSceneFixedState)
    {
        const auto expected = index == 0x2104 ? kSceneDepthOnlyState[0] : index == 0x2208 ? kSceneDepthOnlyState[1]
                                                                                          : value;
        if (registers[index] != expected)
        {
            error = std::format("scene_fixed_state {:#x}: {:#x}", index, registers[index]);
            return false;
        }
    }
    const auto chosen = std::ranges::find_if(pair, [&](const SceneDrawShader* candidate)
                                             {
                                                 const auto& shader = candidate->shader;
                                                 return shader.shaderProgram == registers[0x2180] && shader.shaderContext == registers[0x2181] &&
                                                        NativeGameplayInterpolatorsMatch(shader, registers[0x2182]);
                                             });
    if (chosen == pair.end())
    {
        error = std::format("scene_specialization: depth-only program {:#x} context {:#x} interpolators {:#x}", registers[0x2180], registers[0x2181], registers[0x2182]);
        return false;
    }
    for (const auto& [index, forms, code] : { std::tuple{ 0x2200U, std::span<const std::uint32_t>(kSceneDepthControls), "scene_depth_control" },
                                              std::tuple{ 0x2201U, std::span<const std::uint32_t>(kSceneBlendControls), "scene_blend_control" },
                                              std::tuple{ 0x2205U, std::span<const std::uint32_t>(kSceneRasterControls), "scene_raster_control" } })
        if (std::ranges::find(forms, registers[index]) == forms.end())
        {
            error = std::format("{}: {:#x}", code, registers[index]);
            return false;
        }
    entry = *chosen;
    return true;
}

bool BuildSceneDepthOnlyNative(const NativeGameplayDrawView& view, std::span<const std::uint8_t> effectiveState, const SceneDrawShader& entry, const SceneTargetForm& form, std::vector<NativeGameplayVertexAttribute> attributes, NativeDrawReplayRecipe& native, std::string& error)
{
    return depthOnlyNative(view, effectiveState, entry, form, attributes, native, error, nullptr, nullptr, nullptr);
}

namespace
{

bool depthOnlyNative(const NativeGameplayDrawView& view, std::span<const std::uint8_t> effectiveState, const SceneDrawShader& entry, const SceneTargetForm& form, std::span<const NativeGameplayVertexAttribute> attributes, NativeDrawReplayRecipe& native, std::string& error, SceneBuildCacheState* memo, const GuestBytes* stream, const GuestBytes* indexSource)
{
    try
    {
        auto target = SceneDrawTarget(form);
        if (!DecodeNativeGameplayViewportAndScissor(view.registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < viewport.size(); ++i)
            require(word(effectiveState, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]), "scene_viewport_shadow_scale_mismatch");
        // A vertex shader runs alike at any sample count.
        NativeGameplayShaderInterface        owned;
        const NativeGameplayShaderInterface* shader     = &owned;
        NativeGameplayShaderValidationCache* validation = nullptr;
        if (memo)
        {
            // Draws of an entry decode few layouts into few target forms.
            constexpr std::size_t kMaxSpecializations = 64;
            auto&                 specializations     = memo->Entry(&entry).depthOnly;
            auto                  found               = std::ranges::find_if(specializations, [&](const DepthOnlySpecialization& known)
                                                                             {
                                                  return known.shader.sampleCount == target.sampleCount && known.shader.sampleMask == target.sampleMask &&
                                                         sameAttributes(known.shader.vertexAttributes, attributes);
                                                                             });
            if (found == specializations.end())
            {
                if (specializations.size() >= kMaxSpecializations)
                    specializations.clear();
                auto& made  = specializations.emplace_back();
                made.shader = entry.shader;
                made.shader.vertexAttributes.assign(attributes.begin(), attributes.end());
                made.shader.sampleCount = target.sampleCount;
                made.shader.sampleMask  = target.sampleMask;
                found                   = std::prev(specializations.end());
            }
            shader     = &found->shader;
            validation = &found->validation;
        }
        else
        {
            owned = entry.shader;
            owned.vertexAttributes.assign(attributes.begin(), attributes.end());
            owned.sampleCount = target.sampleCount;
            owned.sampleMask  = target.sampleMask;
        }
        if (!(memo ? memo->BuildNative(view, *stream, *indexSource, *shader, target, native, error, validation) : BuildNativeGameplayDrawRecipe(view, *shader, target, native, error)))
        {
            error = "scene_recipe_rejected: " + error;
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace

bool BuildSceneDraw(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, NativeDrawReplayRecipe& draw, std::string& detail, LeaderTextureCache* cache, SceneBuildCache* build)
{
    detail.clear();
    if (!inputs.sceneCandidate)
        return false;
    // Scene recipe scratch retains capacity; each build clears scratch and
    // payloads and resets native output on refusal.
    thread_local SceneDrawRecipe scratch;

    struct Emptied
    {
        SceneDrawRecipe& recipe;

        ~Emptied()
        {
            recipe.patchedVertexCode.clear();
            recipe.effectiveState.clear();
            recipe.textures.clear();
            recipe.samplers.clear();
            recipe.entry = nullptr;
            recipe.form  = nullptr;
            recipe.pair.clear();
        }
    } const emptied{ scratch };

    if (!buildSceneDrawRecipe(inputs, catalog, scratch, detail, cache, build))
    {
        scratch.native = {};
        if (detail.empty())
            detail = "scene_recipe_rejected";
        return false;
    }
    // Move construction skips the move assignment's release of every slot.
    static_assert(std::is_nothrow_move_constructible_v<NativeDrawReplayRecipe>);
    std::destroy_at(&draw);
    std::construct_at(&draw, std::move(scratch.native));
    return true;
}

namespace
{

std::string ScenePatchedVertexDigest(const IndexedBufferDrawInputs& inputs, std::uint32_t stride)
{
    const auto identity = depthOnlyIdentity(inputs, stride);
    return identity->ok ? identity->digest : std::string();
}

} // namespace

NativeDrawFamily MakeSceneDrawFamily(ReadMemory read, std::shared_ptr<const SceneDrawCatalog> catalog)
{
    auto cache = std::make_shared<LeaderTextureCache>();
    auto build = std::make_shared<SceneBuildCache>(catalog);
    // The reader's digest views borrow from the catalog it holds.
    return MakeReaderDrawFamily<IndexedBufferDrawInputs>([read = std::move(read), catalog, shaders = ScenePixelShaders(*catalog), depthOnly = SceneDepthOnlyVertexShaders(*catalog), patchedDigest = PatchedVertexDigest(ScenePatchedVertexDigest)](const NativeDrawSubmission& submission, IndexedBufferDrawInputs& inputs, std::string& detail)
                                                         {
                                                             const auto form = submission.hook == NativeDrawHook::IndexedUpLowerer ? IndexedBufferForm::Particle : IndexedBufferForm::Scene;
                                                             if (ReadIndexedBufferDrawInputs(submission, read, shaders, inputs, detail, form, depthOnly, patchedDigest))
                                                                 return true;
                                                             // A foreign hook or caller reads nothing; a scene draw outside
                                                             // the catalog is named so a strict run reports it.
                                                             if (!detail.empty() || inputs.state.size() != 0x3500)
                                                                 return false;
                                                             if (!inputs.pixelObject)
                                                                 detail = "scene_no_pixel_shader";
                                                             else if (!inputs.vertexObject)
                                                                 detail = "scene_no_vertex_shader";
                                                             else if (!inputs.pixelCode.empty() && inputs.pixelCode.size() % 4 == 0)
                                                                 detail = "scene_catalog_pixel: " + IndexedBufferMicrocodeWordDigest(inputs.pixelCode).substr(0, 16);
                                                             return false;
                                                         },
                                                         [catalog, cache, build](const IndexedBufferDrawInputs& inputs, NativeDrawReplayRecipe& draw, std::string& detail)
                                                         {
                                                             return BuildSceneDraw(inputs, *catalog, draw, detail, cache.get(), build.get());
                                                         });
}

NativeDrawFamilyRegistration SceneDrawFamily()
{
    return { "scene catalog", "native_draw_scene_shaders", "scene shader catalog", [](const std::filesystem::path& source, ReadMemory read, NativeDrawFamily& family, std::string& summary, std::string& error)
             {
                 auto catalog = std::make_shared<SceneDrawCatalog>();
                 if (!LoadSceneDrawCatalog(source, *catalog, error))
                     return false;
                 summary = std::format("({} specializations of {} pixel shaders, {} depth-only)", catalog->entries.size(), ScenePixelShaders(*catalog).size(), catalog->depthOnly.size());
                 family  = MakeSceneDrawFamily(std::move(read), std::move(catalog));
                 return true;
             } };
}

} // namespace rerevved::gpu::guest_d3d
