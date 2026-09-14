#include "native_guest_draw_capture.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <rex/crypto/sha256.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_texture_upload.h"

REX_EXTERN(__imp__sub_826A3568);
REX_EXTERN(__imp__sub_826A3000);
REX_EXTERN(__imp__sub_826AD150);

namespace rerevved::gpu::diagnostics
{
namespace
{

constexpr std::uint32_t kStateBytes       = 0x3500;
constexpr std::uint32_t kMaxGeometryBytes = 16U * 1024U * 1024U;

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
    std::thread::id                        owner;
    std::array<std::uint32_t, 3>           vertexIdentity{};
    std::vector<std::uint8_t>              vertexMicrocode;
};

struct Capture
{
    std::shared_ptr<Session>  session;
    std::filesystem::path     directory;
    toml::table               metadata;
    std::uint32_t             graphics = 0, cursor = 0;
    std::uint32_t             vertexSize = 0, indexSize = 0;
    std::vector<std::uint8_t> vertices, indices, vertexMicrocode;
    NativeTextureFetch        fetch{};
    NativeDrawReplayTexture   texture;
    NativeGuestMenuDraw       draw;
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

std::unique_ptr<Capture> before(const PPCContext& ctx, bool indexed = true)
{
    if (!enabled.load(std::memory_order_acquire))
        return {};
    // 0x82304254 submits the expanded label triangles through the nonindexed
    // UP wrapper. Other callers of that wrapper are outside this capture.
    if (!indexed && ctx.lr != 0x82304258)
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
        if (session->frameConsumer && session->owner != std::this_thread::get_id())
            throw std::runtime_error("guest UI draws and swap must share one CPU thread");
        ++session->calls;
        if (!session->frameConsumer && session->calls == 256)
            enabled.store(false, std::memory_order_release);
        // The two admitted GFx call sites use the indexed UP wrapper. Its raw
        // hook retains the original stack argument and all original effects.
        if ((indexed && ctx.lr != 0x82303E40 && ctx.lr != 0x82303E90) || ctx.r4.u32 != 4)
            throw std::runtime_error("unexpected caller at guest menu draw boundary");
        const auto stride = indexed ? word(copyMemory(std::uint64_t(ctx.r1.u32) + 84, 4).data()) : ctx.r7.u32;
        if ((indexed && stride != 4 && stride != 8 && stride != 12 && stride != 28) || (!indexed && stride != 28))
            throw std::runtime_error("unsupported guest menu vertex stride");
        const auto         minimumVertex = indexed ? ctx.r5.u32 : 0U;
        const auto         vertexCount   = indexed ? ctx.r6.u32 : ctx.r5.u32;
        const auto         indexCount    = indexed ? ctx.r7.u32 : 0U;
        const auto         indexAddress  = indexed ? ctx.r8.u32 : 0U;
        const auto         indexFormat   = indexed ? ctx.r9.u32 : 0U;
        const auto         vertexAddress = indexed ? ctx.r10.u32 : ctx.r6.u32;
        const auto         state         = copyMemory(ctx.r3.u32, kStateBytes);
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
        capture->directory = session->frameConsumer ? session->directory / fmt::format("frame-{:04}", session->frame) /
                                                          fmt::format("draw-{:04}", session->frameDraws)
                                                    : session->directory / fmt::format("draw-{:04}", session->captures);
        ++session->captures;
        ++session->frameDraws;
        std::filesystem::create_directory(capture->directory);
        capture->fetch    = fetch;
        capture->graphics = ctx.r3.u32;
        capture->cursor   = word(state.data() + 48);
        capture->metadata = toml::table{
            { "schema", 1 }, { "caller", static_cast<std::int64_t>(ctx.lr) }, { "graphics", ctx.r3.u32 }, { "primitive", ctx.r4.u32 }, { "indexed", indexed }, { "minimum_vertex", minimumVertex }, { "vertex_count", vertexCount }, { "index_count", indexCount }, { "index_address", indexAddress }, { "index_format", indexFormat }, { "vertex_address", vertexAddress }, { "stride", stride }, { "cursor_before", capture->cursor }, { "original_returned", false }
        };
        save(*capture, "state-before.be.bin", state);
        const auto vertexSize = std::uint64_t(vertexCount) * stride;
        const auto indexSize  = std::uint64_t(indexCount) * ((indexFormat & 4) ? 4 : 2);
        if (!vertexSize || vertexSize > kMaxGeometryBytes || (indexed && !indexSize) || indexSize > kMaxGeometryBytes)
            throw std::runtime_error("guest menu geometry exceeds capture bounds");
        capture->vertexSize = static_cast<std::uint32_t>(vertexSize);
        capture->indexSize  = static_cast<std::uint32_t>(indexSize);
        capture->vertices   = copyMemory(std::uint64_t(vertexAddress) + std::uint64_t(minimumVertex) * stride, vertexSize);
        if (indexed)
            capture->indices = copyMemory(indexAddress, indexSize);
        capture->draw.indexed       = indexed;
        capture->draw.primitive     = ctx.r4.u32;
        capture->draw.minimumVertex = minimumVertex;
        capture->draw.vertexCount   = vertexCount;
        capture->draw.indexCount    = indexCount;
        capture->draw.indexFormat   = indexFormat;
        capture->draw.stride        = stride;
        save(*capture, "vertex.bin", capture->vertices);
        save(*capture, "index.bin", capture->indices);
        NativeTextureMemoryRange range;
        auto&                    texture = capture->texture;
        std::string              error;
        if (GetNativeTextureMemoryRange(fetch, range, error))
        {
            const auto source = copyMemory(range.address, range.size, true);
            if (!DecodeNativeTexture(fetch, source, texture, error))
                throw std::runtime_error(error);
            save(*capture, "texture-guest.bin", source);
            save(*capture, "texture-native.bin", texture.bytes);
            toml::array swizzle;
            for (const auto component : texture.swizzle)
                swizzle.push_back(component);
            capture->metadata.insert("texture", toml::table{ { "address", range.address }, { "width", texture.width }, { "height", texture.height }, { "format", static_cast<std::int64_t>(texture.format) }, { "swizzle", std::move(swizzle) } });
        }
        else
            capture->metadata.insert("texture_error", error);
        session->ownedBytes += capture->vertices.size() + capture->indices.size() + texture.bytes.size() + kStateBytes;
        if (session->ownedBytes > 512U * 1024U * 1024U)
            throw std::runtime_error("guest UI inputs exceed session byte bound");
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

void after(Capture& capture, const PPCContext& ctx)
{
    std::lock_guard lock(captureMutex);
    if (session != capture.session || session->failed)
        return;
    try
    {
        const auto state = copyMemory(capture.graphics, kStateBytes);
        save(capture, "state-after.be.bin", state);
        const auto cursor = word(state.data() + 48);
        // A successful wrapper returns memcpy's destination, not an HRESULT.
        // Preserve raw completion observations instead of inferring success.
        capture.metadata.insert_or_assign("original_returned", true);
        capture.metadata.insert("r3_after", ctx.r3.u32);
        capture.metadata.insert("cursor_after", cursor);
        const auto vertexOutput = word(state.data() + 0x3488);
        const auto indexOutput  = word(state.data() + 0x348C);
        capture.metadata.insert("vertex_output", vertexOutput);
        capture.metadata.insert("index_output", indexOutput);
        const auto output    = capture.draw.indexed ? indexOutput : vertexOutput;
        const bool completed = output && ctx.r3.u32 == output && cursor == word(state.data() + 0x3484);
        if (completed)
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
                throw std::runtime_error("guest UI draw did not complete its upload and shader selection");
            for (std::size_t i = 0; i < capture.fetch.size(); ++i)
                if (capture.fetch[i] != word(state.data() + 0x480 + i * 4))
                    throw std::runtime_error("guest texture binding changed during the draw");
            capture.draw.state           = state;
            capture.draw.vertices        = capture.vertices;
            capture.draw.indices         = capture.indices;
            capture.draw.vertexMicrocode = capture.vertexMicrocode;
            capture.draw.pixelMicrocode  = pixelMicrocode;
            capture.draw.texture         = &capture.texture;
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
            throw std::runtime_error("guest UI replay requires draw and frame consumers together");
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

void NotifyNativeGuestFrameBoundary()
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
            session->owner = std::this_thread::get_id();
            session->armed = true;
        }
        else
        {
            if (session->owner != std::this_thread::get_id() || !session->frameDraws || session->finished != session->captures)
                throw std::runtime_error("guest UI frame is empty, interleaved, or has unfinished draws");
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
    auto  capture                             = rerevved::gpu::diagnostics::before(ctx, false);
    auto* previous                            = rerevved::gpu::diagnostics::activeCapture;
    rerevved::gpu::diagnostics::activeCapture = capture.get();
    __imp__sub_826A3000(ctx, base);
    rerevved::gpu::diagnostics::activeCapture = previous;
    if (capture)
        rerevved::gpu::diagnostics::after(*capture, ctx);
}
