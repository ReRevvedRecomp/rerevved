#include "native_guest_draw_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <rex/crypto/sha256.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_texture_upload.h"

REX_EXTERN(__imp__sub_826A3568);
REX_EXTERN(__imp__sub_826A3000);
REX_EXTERN(__imp__sub_826A39F8);
REX_EXTERN(__imp__sub_826AD150);

namespace rerevved::gpu::diagnostics
{
namespace
{

constexpr std::uint32_t kStateBytes       = 0x3500;
constexpr std::uint32_t kMaxGeometryBytes = 16U * 1024U * 1024U;

enum class DrawSource
{
    IndexedUp,
    NonindexedUp,
    IndexedBuffer,
};

struct Session
{
    std::filesystem::path                  directory;
    std::chrono::steady_clock::time_point  nextPoll{};
    bool                                   armed = false, failed = false;
    std::uint32_t                          calls = 0, captures = 0, finished = 0;
    std::set<std::array<std::uint32_t, 5>> layouts;
    NativeGuestDrawConsumer                consumer;
    NativeGuestFrameConsumer               frameConsumer;
    std::uint32_t                          frame = 0, frameDraws = 0;
    std::uint64_t                          ownedBytes = 0;
    bool                                   complete   = false;
    std::uint32_t                          graphics   = 0;
    std::array<std::uint32_t, 2>           clearWords{};
    toml::table                            frameMetadata;
    std::array<std::uint32_t, 3>           vertexIdentity{};
    std::vector<std::uint8_t>              vertexMicrocode;
};

struct Capture
{
    std::shared_ptr<Session>               session;
    std::filesystem::path                  directory;
    toml::table                            metadata;
    std::uint32_t                          graphics = 0, cursor = 0;
    std::uint32_t                          frame      = 0;
    std::uint32_t                          vertexSize = 0, indexSize = 0;
    std::uint32_t                          vertexAddress = 0, indexAddress = 0;
    std::vector<std::uint8_t>              vertices, indices, vertexMicrocode;
    std::array<NativeTextureFetch, 3>      fetches{};
    std::array<NativeDrawReplayTexture, 3> textures;
    NativeGuestMenuDraw                    draw;
    DrawSource                             source          = DrawSource::IndexedUp;
    std::uint32_t                          submittedCursor = 0, submittedIndex = 0, submittedSize = 0;
    std::uint32_t                          submissions = 0;
};

std::mutex               captureMutex;
std::shared_ptr<Session> session;
std::atomic<bool>        enabled{ false };
thread_local Capture*    activeCapture = nullptr;

std::uint32_t word(const std::uint8_t* bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

std::string hostThread()
{
    std::ostringstream text;
    text << std::this_thread::get_id();
    return text.str();
}

std::vector<std::uint8_t> copyMemory(std::uint64_t address, std::uint64_t size, bool physical = false)
{
    const auto ceiling = physical ? 0x20000000ULL : 0x100000000ULL;
    if (!size || size > 64U * 1024U * 1024U || address >= ceiling || size > ceiling - address)
        throw std::runtime_error("guest draw input exceeds address or allocation bounds");
    auto*      memory = REX_KERNEL_MEMORY();
    auto*      heap   = physical ? memory->GetPhysicalHeap() : memory->LookupHeap(static_cast<std::uint32_t>(address));
    const auto last   = static_cast<std::uint32_t>(address + size - 1);
    if (!heap || (!physical && memory->LookupHeap(last) != heap) ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(static_cast<std::uint32_t>(address), last)) &
          static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        throw std::runtime_error("guest draw input is not committed readable memory");
    const auto* data = physical ? memory->TranslatePhysical<const std::uint8_t*>(static_cast<std::uint32_t>(address))
                                : memory->TranslateVirtual<const std::uint8_t*>(static_cast<std::uint32_t>(address));
    return { data, data + size };
}

void save(Capture& capture, const char* name, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(capture.directory / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if (!file)
        throw std::runtime_error("could not save guest draw input");
    capture.metadata.insert(name, toml::table{ { "size", static_cast<std::int64_t>(bytes.size()) }, { "sha256", rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())) } });
}

void writeMetadata(const std::filesystem::path& path, const toml::table& metadata)
{
    std::ofstream file(path);
    file << metadata;
    file.close();
    if (!file)
        throw std::runtime_error("could not save guest draw metadata");
}

void fail(const std::exception& exception) noexcept
{
    enabled.store(false, std::memory_order_release);
    REXLOG_ERROR("Native guest draw capture failed: {}", exception.what());
    try
    {
        if (session)
        {
            session->failed = true;
            writeMetadata(session->directory / "result.toml",
                          toml::table{ { "complete", false }, { "error", exception.what() } });
        }
    }
    catch (...)
    {
    }
}

void finishIfReady()
{
    if (!session->frameConsumer && session->calls == 256 && session->finished == session->captures)
    {
        writeMetadata(session->directory / "result.toml", toml::table{ { "complete", true }, { "calls", 256 }, { "captures", session->finished } });
        session->complete = true;
    }
}

std::unique_ptr<Capture> before(const PPCContext& ctx, DrawSource source = DrawSource::IndexedUp)
{
    if (!enabled.load(std::memory_order_acquire))
        return {};
    // Labels and the movie submit their CPU vertices through this wrapper.
    const bool indexed = source != DrawSource::NonindexedUp;
    const bool bound   = source == DrawSource::IndexedBuffer;
    const bool movie   = !indexed && ctx.lr == 0x82E3987C;
    if (!indexed && ctx.lr != 0x82304258 && !movie)
        return {};
    std::lock_guard lock(captureMutex);
    if (!session || !enabled.load(std::memory_order_relaxed))
        return {};
    try
    {
        if (!session->armed)
        {
            if (session->frameConsumer)
                return {};
            const auto now = std::chrono::steady_clock::now();
            if (now < session->nextPoll)
                return {};
            session->nextPoll = now + std::chrono::milliseconds(100);
            if (!std::filesystem::is_regular_file(session->directory / "arm"))
                return {};
            session->armed = true;
        }
        // The loading swap worker differs from the draw thread. The device and
        // interval must still agree, with no original draw left in flight.
        if (session->frameConsumer &&
            (ctx.r3.u32 != session->graphics || session->captures != session->finished))
            throw std::runtime_error("guest frame draw has a foreign device or overlaps an unfinished draw");
        ++session->calls;
        if (!session->frameConsumer && session->calls == 256)
            enabled.store(false, std::memory_order_release);
        // The two admitted GFx call sites use the indexed UP wrapper. Its raw
        // hook retains the original stack argument and all original effects.
        if ((!bound && indexed && ctx.lr != 0x82303E40 && ctx.lr != 0x82303E90) ||
            ctx.r4.u32 != (movie ? 6U : 4U))
            throw std::runtime_error("unexpected caller at guest menu draw boundary");
        if (bound && (ctx.r5.u32 || ctx.r6.u32 || (ctx.r7.u32 != 12 && ctx.r7.u32 != 1536)))
            throw std::runtime_error("unsupported guest scene base, start or index count");
        const auto stride = bound ? 32U : indexed ? word(copyMemory(std::uint64_t(ctx.r1.u32) + 84, 4).data())
                                                  : ctx.r7.u32;
        if ((!bound && indexed && stride != 4 && stride != 8 && stride != 12 && stride != 28) ||
            (!indexed && stride != (movie ? 20U : 28U)) ||
            (movie && (ctx.r5.u32 != 4 || std::uint64_t(ctx.r6.u32) != std::uint64_t(ctx.r1.u32) + 0x70)))
            throw std::runtime_error("unsupported guest menu vertex stride");
        const auto minimumVertex = indexed ? ctx.r5.u32 : 0U;
        auto       vertexCount   = indexed ? ctx.r6.u32 : ctx.r5.u32;
        const auto indexCount    = indexed ? ctx.r7.u32 : 0U;
        auto       indexAddress  = indexed ? ctx.r8.u32 : 0U;
        const auto indexFormat   = bound ? 1U : indexed ? ctx.r9.u32
                                                        : 0U;
        auto       vertexAddress = indexed ? ctx.r10.u32 : ctx.r6.u32;
        const auto state         = copyMemory(ctx.r3.u32, kStateBytes);
        if (session->frameConsumer && !session->frameDraws &&
            (word(state.data() + 0x2A30) != session->clearWords[0] ||
             word(state.data() + 0x2A34) != session->clearWords[1]))
            throw std::runtime_error("guest frame clear state changed after the opening swap boundary");
        std::vector<std::uint8_t> indexHeader;
        if (bound)
        {
            // 0x826ACEA0 emits the fetch array at G+0x480 in six-word groups.
            // The pinned scene VS reads the last pair (logical vf0, fetch 95).
            const auto address = word(state.data() + 0x778);
            const auto size    = word(state.data() + 0x77C);
            const auto bytes   = ((size >> 2) & 0xFFFFFFU) * 4U;
            if ((address & 3) != 3 || (size & 3) != 2 ||
                !((indexCount == 12 && bytes == 256) || (indexCount == 1536 && bytes == 9248)))
                throw std::runtime_error("unsupported guest scene vertex fetch");
            vertexAddress = address & ~3U;
            vertexCount   = bytes / stride;
            // 0x826A3C9C uses the bound resource's +0 flags and +24 backing.
            // Only its BE16, zero-start stream is admitted here.
            indexHeader = copyMemory(word(state.data() + 0x3094), 28);
            if ((word(indexHeader.data()) & 0xE0000000U) != 0x20000000U)
                throw std::runtime_error("unsupported guest scene index resource");
            const auto raw = word(indexHeader.data() + 24);
            indexAddress   = (raw & 0x1FFFFFFFU) + (((raw >> 20) + 512U) & 0x1000U);
        }
        NativeTextureFetch fetch;
        for (std::size_t i = 0; i < fetch.size(); ++i)
            fetch[i] = word(state.data() + 0x480 + i * 4);
        const std::array<std::uint32_t, 5> layout{ stride, fetch[1] & 0xFF, fetch[2], fetch[0] >> 31, indexed ? 1U : 0U };
        if (!session->frameConsumer && session->layouts.contains(layout))
        {
            finishIfReady();
            return {};
        }
        if ((!session->frameConsumer && session->captures >= 16) || session->frameDraws >= 256)
            throw std::runtime_error("guest menu layout count exceeds capture bound");
        session->layouts.insert(layout);
        auto capture       = std::make_unique<Capture>();
        capture->session   = session;
        capture->source    = source;
        capture->directory = session->frameConsumer ? session->directory / fmt::format("frame-{:04}", session->frame) /
                                                          fmt::format("draw-{:04}", session->frameDraws)
                                                    : session->directory / fmt::format("draw-{:04}", session->captures);
        ++session->captures;
        ++session->frameDraws;
        std::filesystem::create_directory(capture->directory);
        capture->graphics      = ctx.r3.u32;
        capture->frame         = session->frame;
        capture->vertexAddress = vertexAddress;
        capture->indexAddress  = indexAddress;
        capture->cursor        = word(state.data() + 48);
        capture->metadata      = toml::table{
            { "schema", 1 }, { "caller", static_cast<std::int64_t>(ctx.lr) }, { "graphics", ctx.r3.u32 }, { "primitive", ctx.r4.u32 }, { "indexed", indexed }, { "minimum_vertex", minimumVertex }, { "vertex_count", vertexCount }, { "index_count", indexCount }, { "index_address", indexAddress }, { "index_format", indexFormat }, { "vertex_address", vertexAddress }, { "stride", stride }, { "cursor_before", capture->cursor }, { "original_returned", false }
        };
        capture->metadata.insert("frame_epoch", capture->frame);
        capture->metadata.insert("host_thread", hostThread());
        save(*capture, "state-before.be.bin", state);
        if (bound)
        {
            capture->metadata.insert("bound_indexed", true);
            capture->metadata.insert("index_resource", word(state.data() + 0x3094));
            save(*capture, "index-header.be.bin", indexHeader);
        }
        const auto vertexSize = std::uint64_t(vertexCount) * stride;
        const auto indexSize  = std::uint64_t(indexCount) * ((indexFormat & 4) ? 4 : 2);
        if (!vertexSize || vertexSize > kMaxGeometryBytes || (indexed && !indexSize) || indexSize > kMaxGeometryBytes)
            throw std::runtime_error("guest menu geometry exceeds capture bounds");
        capture->vertexSize = static_cast<std::uint32_t>(vertexSize);
        capture->indexSize  = static_cast<std::uint32_t>(indexSize);
        capture->vertices   = copyMemory(std::uint64_t(vertexAddress) + std::uint64_t(minimumVertex) * stride, vertexSize, bound);
        if (indexed)
            capture->indices = copyMemory(indexAddress, indexSize, bound);
        capture->draw.indexed      = indexed;
        capture->draw.firstInFrame = session->frameConsumer && session->frameDraws == 1;
        capture->metadata.insert("first_in_frame", capture->draw.firstInFrame);
        capture->draw.primitive     = ctx.r4.u32;
        capture->draw.minimumVertex = minimumVertex;
        capture->draw.vertexCount   = vertexCount;
        capture->draw.indexCount    = indexCount;
        capture->draw.indexFormat   = indexFormat;
        capture->draw.stride        = stride;
        save(*capture, "vertex.bin", capture->vertices);
        save(*capture, "index.bin", capture->indices);
        const auto                        textureCount = movie ? 3U : 1U;
        constexpr std::array<unsigned, 3> movieFetches{ 2, 0, 1 };
        toml::array                       textureMetadata;
        for (unsigned slot = 0; slot < textureCount; ++slot)
        {
            const auto fetchIndex = movie ? movieFetches[slot] : 0;
            auto&      input      = capture->fetches[slot];
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = word(state.data() + 0x480 + fetchIndex * 24 + i * 4);
            NativeTextureMemoryRange range;
            auto&                    texture = capture->textures[slot];
            std::string              error;
            if (GetNativeTextureMemoryRange(input, range, error))
            {
                const auto sourceBytes = copyMemory(range.address, range.size, true);
                if (!DecodeNativeTexture(input, sourceBytes, texture, error))
                    throw std::runtime_error(error);
                const auto prefix = movie ? "texture-" + std::to_string(slot) : "texture";
                save(*capture, (prefix + "-guest.bin").c_str(), sourceBytes);
                save(*capture, (prefix + "-native.bin").c_str(), texture.bytes);
                toml::array swizzle;
                for (const auto component : texture.swizzle)
                    swizzle.push_back(component);
                toml::table description{ { "address", range.address }, { "width", texture.width }, { "height", texture.height }, { "format", static_cast<std::int64_t>(texture.format) }, { "swizzle", std::move(swizzle) } };
                if (movie)
                {
                    description.insert("fetch", fetchIndex);
                    textureMetadata.push_back(std::move(description));
                }
                else
                    capture->metadata.insert("texture", std::move(description));
            }
            else if (movie)
                throw std::runtime_error(error);
            else
                capture->metadata.insert("texture_error", error);
            session->ownedBytes += texture.bytes.size();
        }
        if (movie)
            capture->metadata.insert("textures", std::move(textureMetadata));
        session->ownedBytes += capture->vertices.size() + capture->indices.size() + kStateBytes;
        if (session->ownedBytes > 512U * 1024U * 1024U)
            throw std::runtime_error("guest menu inputs exceed session byte bound");
        return capture;
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
    return {};
}

void patchedShader(Capture& capture, std::uint32_t shader, std::uint32_t code, std::uint32_t patchState, std::uint32_t variant, std::uint64_t caller)
{
    std::lock_guard lock(captureMutex);
    if (session != capture.session || session->failed)
        return;
    try
    {
        // Persistent variants are observed at the emitter's selection point.
        if (caller == 0x826ADD10)
            return;
        if (caller != 0x826ADB70 || std::uint64_t(capture.graphics) + 0x30F0 != patchState || variant > 1)
            throw std::runtime_error("unsupported guest vertex shader patch boundary");
        // 0x826ADA38 copies this variant's code, then passes that exact buffer
        // to 0x826AD150 for CPU fetch patching before committing the cursor.
        const auto relative        = copyMemory(std::uint64_t(shader) + 896 + variant * 8, 4);
        const auto metadataAddress = std::uint64_t(shader) + word(relative.data()) + 872;
        const auto metadata        = copyMemory(metadataAddress, 8);
        const auto size            = word(metadata.data() + 4);
        if (!size || size > 64 * 1024 || size % 4)
            throw std::runtime_error("guest vertex shader exceeds capture bounds");
        capture.vertexMicrocode = copyMemory(code, size);
        capture.metadata.insert_or_assign("vertex_shader", toml::table{ { "object", shader }, { "variant", variant }, { "patched_code", code }, { "metadata", static_cast<std::int64_t>(metadataAddress) }, { "patch_return", static_cast<std::int64_t>(caller) } });
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
}

void selectedShader(Capture& capture, std::uint32_t graphics, std::uint32_t shader, std::uint32_t conditional, std::uint32_t variant)
{
    std::lock_guard lock(captureMutex);
    if (session != capture.session || session->failed)
        return;
    try
    {
        if (graphics != capture.graphics || conditional != UINT32_MAX)
            throw std::runtime_error("unsupported conditional guest vertex shader selection");
        if (variant == UINT32_MAX)
            return;
        if (variant > 1)
            throw std::runtime_error("unsupported persistent guest vertex shader variant");
        // 0x826AE558..0x826AE5B4 emits the selected persistent program pointer
        // and word count. Copy the same program before the emitter returns.
        const auto relative        = copyMemory(std::uint64_t(shader) + 896 + variant * 8, 4);
        const auto metadataAddress = std::uint64_t(shader) + word(relative.data()) + 872;
        const auto metadata        = copyMemory(metadataAddress, 8);
        const auto resource        = copyMemory(std::uint64_t(shader) + 32, 4);
        const auto raw             = std::uint64_t(word(resource.data())) + word(metadata.data());
        const auto size            = word(metadata.data() + 4);
        if (raw > UINT32_MAX || !size || size > 64 * 1024 || size % 4)
            throw std::runtime_error("persistent guest vertex shader exceeds capture bounds");
        const auto physical     = (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
        capture.vertexMicrocode = copyMemory(physical, size, true);
        capture.metadata.insert_or_assign("vertex_shader", toml::table{ { "object", shader }, { "variant", variant }, { "physical_code", static_cast<std::int64_t>(physical) }, { "metadata", static_cast<std::int64_t>(metadataAddress) }, { "selection", 0x826AE5B8LL } });
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
}

void indexedSubmission(Capture& capture, std::uint32_t graphics, std::uint32_t cursor, std::uint32_t index, std::uint32_t size, std::uint32_t count, std::uint32_t requested)
{
    std::lock_guard lock(captureMutex);
    if (session != capture.session || session->failed || capture.source != DrawSource::IndexedBuffer)
        return;
    try
    {
        // 0x826A3E18 follows the indexed packet stores, immediately before
        // publishing its final word through G+0x30. Large split draws are excluded.
        if (graphics != capture.graphics || ++capture.submissions != 1 ||
            count != capture.draw.indexCount || requested != count)
            throw std::runtime_error("unsupported guest scene submission boundary");
        capture.submittedCursor = cursor;
        capture.submittedIndex  = index;
        capture.submittedSize   = size;
        capture.metadata.insert("submission", toml::table{ { "boundary", 0x826A3E18LL }, { "cursor", cursor }, { "index_address", index }, { "index_size_word", size }, { "index_count", count } });
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
}

void after(Capture& capture, const PPCContext& ctx)
{
    std::lock_guard lock(captureMutex);
    if (session != capture.session || session->failed)
        return;
    try
    {
        if (session->frameConsumer &&
            (capture.graphics != session->graphics || capture.frame != session->frame ||
             session->captures != session->finished + 1))
            throw std::runtime_error("guest draw returned outside its device or frame interval");
        const auto state = copyMemory(capture.graphics, kStateBytes);
        save(capture, "state-after.be.bin", state);
        if (capture.draw.firstInFrame &&
            (word(state.data() + 0x2A30) != session->clearWords[0] ||
             word(state.data() + 0x2A34) != session->clearWords[1]))
            throw std::runtime_error("guest frame clear state changed during the first original draw");
        const auto cursor = word(state.data() + 48);
        // UP wrappers return memcpy's destination. Bound draws instead publish
        // the cursor observed after the indexed packet's stores.
        capture.metadata.insert_or_assign("original_returned", true);
        capture.metadata.insert("r3_after", ctx.r3.u32);
        capture.metadata.insert("cursor_after", cursor);
        const auto vertexOutput = word(state.data() + 0x3488);
        const auto indexOutput  = word(state.data() + 0x348C);
        capture.metadata.insert("vertex_output", vertexOutput);
        capture.metadata.insert("index_output", indexOutput);
        const bool bound     = capture.source == DrawSource::IndexedBuffer;
        const auto output    = capture.draw.indexed ? indexOutput : vertexOutput;
        const bool completed = bound ? capture.submissions == 1 && cursor == capture.submittedCursor &&
                                           capture.submittedIndex == capture.indexAddress &&
                                           capture.submittedSize == (0x40000000U | capture.draw.indexCount)
                                     : output && ctx.r3.u32 == output && cursor == word(state.data() + 0x3484);
        if (bound && completed)
        {
            const auto vertices = copyMemory(capture.vertexAddress, capture.vertexSize, true);
            const auto indices  = copyMemory(capture.indexAddress, capture.indexSize, true);
            save(capture, "vertex-after.bin", vertices);
            save(capture, "index-after.bin", indices);
            if (vertices != capture.vertices || indices != capture.indices ||
                word(state.data() + 0x778) != (capture.vertexAddress | 3U) ||
                (((word(state.data() + 0x77C) >> 2) & 0xFFFFFFU) * 4) != capture.vertexSize)
                throw std::runtime_error("guest scene bound geometry changed during the draw");
        }
        if (!bound && completed)
        {
            save(capture, "vertex-output.bin", copyMemory(vertexOutput, capture.vertexSize));
            if (capture.draw.indexed)
                save(capture, "index-output.bin", copyMemory(indexOutput, capture.indexSize));
        }
        if (cursor > capture.cursor && cursor - capture.cursor <= 256U * 1024U)
            save(capture, "commands.be.bin", copyMemory(capture.cursor, cursor - capture.cursor));
        const auto pixelObject          = word(state.data() + 0x3194);
        const auto pixelHeader          = copyMemory(pixelObject, 68);
        const auto pixelMetadataAddress = std::uint64_t(pixelObject) + word(pixelHeader.data() + 64) + 40;
        const auto pixelMetadata        = copyMemory(pixelMetadataAddress, 8);
        const auto rawAddress           = std::uint64_t(word(pixelHeader.data() + 24)) + word(pixelMetadata.data());
        const auto pixelSize            = word(pixelMetadata.data() + 4);
        if (rawAddress > UINT32_MAX || !pixelSize || pixelSize > 64 * 1024 || pixelSize % 4)
            throw std::runtime_error("guest pixel shader exceeds capture bounds");
        // The guest's physical alias calculation in 0x826ADF14, before its
        // pixel-stage tag is applied. copyMemory validates the physical range.
        const auto pixelAddress   = (rawAddress & 0x1FFFFFFEULL) + (((rawAddress >> 20) + 512) & 0x1000);
        const auto pixelMicrocode = copyMemory(pixelAddress, pixelSize, true);
        save(capture, "pixel-header.be.bin", pixelHeader);
        save(capture, "pixel-metadata.be.bin", pixelMetadata);
        save(capture, "pixel-shader.be.bin", pixelMicrocode);
        capture.metadata.insert("pixel_shader", toml::table{ { "object", pixelObject }, { "metadata", static_cast<std::int64_t>(pixelMetadataAddress) }, { "physical_code", static_cast<std::int64_t>(pixelAddress) } });
        const std::array<std::uint32_t, 3> identity{ capture.graphics, word(state.data() + 0x3198), word(state.data() + 0x2E2C) };
        if (capture.vertexMicrocode.empty() && session->frameConsumer && identity == session->vertexIdentity)
        {
            capture.vertexMicrocode = session->vertexMicrocode;
            capture.metadata.insert("vertex_shader_retained", true);
        }
        if (!capture.vertexMicrocode.empty())
        {
            save(capture, "vertex-shader.be.bin", capture.vertexMicrocode);
            session->vertexMicrocode = capture.vertexMicrocode;
            session->vertexIdentity  = identity;
        }
        if (session->consumer)
        {
            if (!completed || capture.vertexMicrocode.empty())
                throw std::runtime_error("guest menu draw did not complete its submission and shader selection");
            const bool                        movie = !capture.draw.indexed && capture.draw.stride == 20;
            constexpr std::array<unsigned, 3> movieFetches{ 2, 0, 1 };
            for (unsigned slot = 0; slot < (movie ? 3U : 1U); ++slot)
            {
                const auto  fetchIndex = movie ? movieFetches[slot] : 0;
                const auto& fetch      = capture.fetches[slot];
                for (std::size_t i = 0; i < fetch.size(); ++i)
                    if (fetch[i] != word(state.data() + 0x480 + fetchIndex * 24 + i * 4))
                        throw std::runtime_error("guest texture binding changed during the draw");
                if (movie)
                {
                    NativeTextureMemoryRange range;
                    NativeDrawReplayTexture  afterTexture;
                    std::string              error;
                    if (!GetNativeTextureMemoryRange(fetch, range, error) ||
                        !DecodeNativeTexture(fetch, copyMemory(range.address, range.size, true), afterTexture, error))
                        throw std::runtime_error(error);
                    if (afterTexture.bytes != capture.textures[slot].bytes)
                        throw std::runtime_error("movie texture bytes changed during the original draw");
                }
                capture.draw.textures[slot] = &capture.textures[slot];
            }
            std::vector<std::uint8_t> vertexLiterals;
            if (bound)
            {
                // 0x826ADB98 reads the shader's literal records and emits their
                // resource backing as LOAD_ALU_CONSTANT, outside the G shadow.
                const auto shader = word(state.data() + 0x3198);
                const auto header = copyMemory(std::uint64_t(shader) + 872, 24);
                save(capture, "vertex-literal-header.be.bin", header);
                const auto relative = word(header.data() + 20);
                if (!relative)
                    throw std::runtime_error("guest scene shader has no literal table");
                const auto tableAddress = std::uint64_t(shader) + 872 + relative;
                const auto table        = copyMemory(tableAddress, 20);
                save(capture, "vertex-literal-table.be.bin", table);
                const auto recordBytes = word(table.data() + 16);
                if (recordBytes < 8 || recordBytes > 128 || recordBytes % 4)
                    throw std::runtime_error("unsupported guest scene literal table");
                const auto record = copyMemory(tableAddress + 20, recordBytes);
                save(capture, "vertex-literal-record.be.bin", record);
                if (word(record.data()) != 0x00FC0010U)
                    throw std::runtime_error("unsupported guest scene literal range");
                // The loader stops at a zero count before reading that record's
                // address. The table may include bytes after this terminator.
                if (recordBytes != 8 && (word(record.data() + 8) & 0xFFFFU))
                    throw std::runtime_error("unsupported additional guest scene literals");
                const auto resource = copyMemory(std::uint64_t(shader) + 32, 4);
                save(capture, "vertex-literal-resource.be.bin", resource);
                const auto raw = std::uint64_t(word(resource.data())) + word(record.data() + 4);
                if (raw > UINT32_MAX)
                    throw std::runtime_error("guest scene literals exceed address bounds");
                const auto physical = (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
                vertexLiterals      = copyMemory(physical, 64, true);
                save(capture, "vertex-literals.be.bin", vertexLiterals);
                capture.metadata.insert("vertex_literals", toml::table{ { "shader", shader }, { "table", static_cast<std::int64_t>(tableAddress) }, { "physical", static_cast<std::int64_t>(physical) }, { "first_constant", 252 }, { "word_count", 16 } });
            }
            std::vector<std::uint8_t> pixelLiterals;
            if (movie)
            {
                // 0x826ADEF8 passes PS+0x28 and *(PS+0x18) to the same
                // literal loader. Its record indices cover the unified ALU file.
                const auto header = copyMemory(std::uint64_t(pixelObject) + 40, 24);
                save(capture, "pixel-literal-header.be.bin", header);
                const auto relative = word(header.data() + 20);
                if (!relative)
                    throw std::runtime_error("movie shader has no pixel literal table");
                const auto tableAddress = std::uint64_t(pixelObject) + 40 + relative;
                const auto table        = copyMemory(tableAddress, 20);
                save(capture, "pixel-literal-table.be.bin", table);
                const auto recordBytes = word(table.data() + 16);
                if (recordBytes < 8 || recordBytes > 128 || recordBytes % 4)
                    throw std::runtime_error("unsupported movie pixel literal table");
                const auto records = copyMemory(tableAddress + 20, recordBytes);
                save(capture, "pixel-literal-records.be.bin", records);
                pixelLiterals.resize(64);
                unsigned    covered = 0;
                toml::array ranges;
                for (std::size_t offset = 0; offset < records.size(); offset += 8)
                {
                    const auto record = word(records.data() + offset);
                    const auto first = record >> 16, count = record & 0xFFFFU;
                    if (!count)
                        break;
                    // The resource block starts at PS c252, unified slot 508.
                    if (offset + 8 > records.size() || first < 508 || first > 511 ||
                        count > (512 - first) * 4)
                        throw std::runtime_error("movie pixel literal range is outside c252 through c255");
                    const auto destination = (first - 508) * 4;
                    const auto mask        = ((1U << count) - 1U) << destination;
                    if (covered & mask)
                        throw std::runtime_error("movie pixel literal ranges overlap");
                    const auto raw = std::uint64_t(word(pixelHeader.data() + 24)) + word(records.data() + offset + 4);
                    if (raw > UINT32_MAX)
                        throw std::runtime_error("movie pixel literals exceed address bounds");
                    const auto physical = (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
                    const auto bytes    = copyMemory(physical, count * 4, true);
                    std::copy(bytes.begin(), bytes.end(), pixelLiterals.begin() + destination * 4);
                    covered |= mask;
                    ranges.push_back(toml::table{ { "first_alu_constant", first }, { "word_count", count }, { "physical", static_cast<std::int64_t>(physical) } });
                }
                if (covered != 0xFFFF)
                    throw std::runtime_error("movie pixel literals do not cover c252 through c255");
                save(capture, "pixel-literals.be.bin", pixelLiterals);
                capture.metadata.insert("pixel_literals", std::move(ranges));
            }
            capture.draw.pixelLiterals   = pixelLiterals;
            capture.draw.vertexLiterals  = vertexLiterals;
            capture.draw.state           = state;
            capture.draw.vertices        = capture.vertices;
            capture.draw.indices         = capture.indices;
            capture.draw.vertexMicrocode = capture.vertexMicrocode;
            capture.draw.pixelMicrocode  = pixelMicrocode;
            std::string error;
            if (!session->consumer(capture.draw, capture.directory, error))
                throw std::runtime_error(error);
            capture.metadata.insert("live_native_draw", true);
        }
        writeMetadata(capture.directory / "manifest.toml", capture.metadata);
        ++session->finished;
        finishIfReady();
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
}

} // namespace

bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error, NativeGuestDrawConsumer consumer, NativeGuestFrameConsumer frameConsumer)
{
    std::lock_guard lock(captureMutex);
    try
    {
        if (bool(consumer) != bool(frameConsumer))
            throw std::runtime_error("guest menu replay requires draw and frame consumers together");
        if (session || !std::filesystem::create_directories(directory))
            throw std::runtime_error("guest draw capture requires a fresh directory and one session");
        session                = std::make_shared<Session>();
        session->directory     = directory;
        session->consumer      = std::move(consumer);
        session->frameConsumer = std::move(frameConsumer);
        enabled.store(true, std::memory_order_release);
        REXLOG_INFO("Native guest draw capture waiting for {}/arm", directory.string());
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void NotifyNativeGuestFrameBoundary(std::uint32_t graphics, std::uint32_t reservation, std::uint32_t descriptor)
{
    if (!enabled.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(captureMutex);
    if (!session || !session->frameConsumer || !enabled.load(std::memory_order_relaxed))
        return;
    try
    {
        if (!session->armed)
        {
            if (!std::filesystem::is_regular_file(session->directory / "arm"))
                return;
            session->graphics = graphics;
            session->armed    = true;
        }
        else
        {
            if (graphics != session->graphics || !session->frameDraws || session->finished != session->captures)
                throw std::runtime_error("guest frame has a foreign device, is empty, or has unfinished draws");
            session->frameMetadata.insert("end", toml::table{ { "host_thread", hostThread() }, { "reservation", reservation }, { "descriptor", descriptor } });
            session->frameMetadata.insert("original_draws_returned", session->frameDraws);
            writeMetadata(session->directory / fmt::format("frame-{:04}", session->frame) / "frame.toml", session->frameMetadata);
            std::string error;
            const bool  last = session->frame == 2;
            if (!session->frameConsumer(session->directory, last, error))
                throw std::runtime_error(error);
            ++session->frame;
            if (last)
            {
                writeMetadata(session->directory / "result.toml", toml::table{ { "complete", true }, { "frames", session->frame }, { "captures", session->finished }, { "calls", session->calls }, { "boundary", 0x826A4884LL } });
                session->complete = true;
                enabled.store(false, std::memory_order_release);
                return;
            }
        }
        session->frameDraws = 0;
        session->vertexMicrocode.clear();
        session->vertexIdentity = {};
        std::filesystem::create_directory(session->directory / fmt::format("frame-{:04}", session->frame));
        // These retained Resolve copy-clear words initialize the bounded native
        // replay. The first draw must observe the same words before and after
        // its original submission; this does not replace the guest Clear API.
        const auto clear       = copyMemory(std::uint64_t(graphics) + 0x2A30, 8);
        session->clearWords    = { word(clear.data()), word(clear.data() + 4) };
        session->frameMetadata = toml::table{ { "frame_epoch", session->frame }, { "graphics", graphics }, { "copy_clear", session->clearWords[0] }, { "copy_clear_low", session->clearWords[1] }, { "begin", toml::table{ { "host_thread", hostThread() }, { "reservation", reservation }, { "descriptor", descriptor } } } };
        writeMetadata(session->directory / fmt::format("frame-{:04}", session->frame) / "frame.toml", session->frameMetadata);
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
}

void StopNativeGuestDrawCapture()
{
    std::lock_guard lock(captureMutex);
    enabled.store(false, std::memory_order_release);
    if (session && !session->failed && !session->complete)
    {
        const std::runtime_error cancelled("guest draw capture stopped before completion");
        fail(cancelled);
    }
    session.reset();
}

} // namespace rerevved::gpu::diagnostics

void ObserveNativeGuestVertexSelection(PPCRegister& r30, PPCRegister& r31, PPCRegister& r15, PPCRegister& r19)
{
    if (auto* capture = rerevved::gpu::diagnostics::activeCapture)
        rerevved::gpu::diagnostics::selectedShader(*capture, r30.u32, r31.u32, r15.u32, r19.u32);
}

void ObserveNativeGuestIndexedSubmission(PPCRegister& r31, PPCRegister& r11, PPCRegister& r28, PPCRegister& r29, PPCRegister& r27, PPCRegister& r19)
{
    if (auto* capture = rerevved::gpu::diagnostics::activeCapture)
        rerevved::gpu::diagnostics::indexedSubmission(*capture, r31.u32, r11.u32, r28.u32, r29.u32, r27.u32, r19.u32);
}

REX_HOOK_RAW(sub_826A39F8)
{
    auto  capture                             = rerevved::gpu::diagnostics::before(ctx, rerevved::gpu::diagnostics::DrawSource::IndexedBuffer);
    auto* previous                            = rerevved::gpu::diagnostics::activeCapture;
    rerevved::gpu::diagnostics::activeCapture = capture.get();
    __imp__sub_826A39F8(ctx, base);
    rerevved::gpu::diagnostics::activeCapture = previous;
    if (capture)
        rerevved::gpu::diagnostics::after(*capture, ctx);
}

REX_HOOK_RAW(sub_826A3568)
{
    auto  capture                             = rerevved::gpu::diagnostics::before(ctx);
    auto* previous                            = rerevved::gpu::diagnostics::activeCapture;
    rerevved::gpu::diagnostics::activeCapture = capture.get();
    __imp__sub_826A3568(ctx, base);
    rerevved::gpu::diagnostics::activeCapture = previous;
    if (capture)
        rerevved::gpu::diagnostics::after(*capture, ctx);
}

REX_HOOK_RAW(sub_826AD150)
{
    auto*      capture = rerevved::gpu::diagnostics::activeCapture;
    const auto shader = ctx.r3.u32, code = ctx.r4.u32, patchState = ctx.r6.u32, variant = ctx.r7.u32;
    const auto caller = ctx.lr;
    __imp__sub_826AD150(ctx, base);
    if (capture)
        rerevved::gpu::diagnostics::patchedShader(*capture, shader, code, patchState, variant, caller);
}

REX_HOOK_RAW(sub_826A3000)
{
    auto  capture                             = rerevved::gpu::diagnostics::before(ctx, rerevved::gpu::diagnostics::DrawSource::NonindexedUp);
    auto* previous                            = rerevved::gpu::diagnostics::activeCapture;
    rerevved::gpu::diagnostics::activeCapture = capture.get();
    __imp__sub_826A3000(ctx, base);
    rerevved::gpu::diagnostics::activeCapture = previous;
    if (capture)
        rerevved::gpu::diagnostics::after(*capture, ctx);
}
