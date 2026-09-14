#include "native_guest_draw_capture.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>

#include <rex/crypto/sha256.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_texture_upload.h"

REX_EXTERN(__imp__sub_826A3568);

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
    std::set<std::array<std::uint32_t, 4>> layouts;
};

struct Capture
{
    std::shared_ptr<Session> session;
    std::filesystem::path    directory;
    toml::table              metadata;
    std::uint32_t            graphics = 0, cursor = 0;
    std::uint32_t            vertexSize = 0, indexSize = 0;
};

std::mutex               captureMutex;
std::shared_ptr<Session> session;
std::atomic<bool>        enabled{ false };

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
    if (session->calls == 256 && session->finished == session->captures)
        writeMetadata(session->directory / "result.toml", toml::table{ { "complete", true }, { "calls", 256 }, { "captures", session->finished } });
}

std::unique_ptr<Capture> before(const PPCContext& ctx)
{
    if (!enabled.load(std::memory_order_acquire))
        return {};
    std::lock_guard lock(captureMutex);
    if (!session || !enabled.load(std::memory_order_relaxed))
        return {};
    try
    {
        if (!session->armed)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < session->nextPoll)
                return {};
            session->nextPoll = now + std::chrono::milliseconds(100);
            if (!std::filesystem::is_regular_file(session->directory / "arm"))
                return {};
            session->armed = true;
        }
        if (++session->calls == 256)
            enabled.store(false, std::memory_order_release);
        // The two admitted GFx call sites use the indexed UP wrapper. Its raw
        // hook retains the original stack argument and all original effects.
        if ((ctx.lr != 0x82303E40 && ctx.lr != 0x82303E90) || ctx.r4.u32 != 4)
            throw std::runtime_error("unexpected caller at guest menu draw boundary");
        const auto strideBytes = copyMemory(std::uint64_t(ctx.r1.u32) + 84, 4);
        const auto stride      = word(strideBytes.data());
        if (stride != 4 && stride != 8 && stride != 12 && stride != 28)
            throw std::runtime_error("unsupported guest menu vertex stride");
        const auto         state = copyMemory(ctx.r3.u32, kStateBytes);
        NativeTextureFetch fetch;
        for (std::size_t i = 0; i < fetch.size(); ++i)
            fetch[i] = word(state.data() + 0x480 + i * 4);
        const std::array<std::uint32_t, 4> layout{ stride, fetch[1] & 0xFF, fetch[2], fetch[0] >> 31 };
        if (session->layouts.contains(layout))
        {
            finishIfReady();
            return {};
        }
        if (session->captures >= 16)
            throw std::runtime_error("guest menu layout count exceeds capture bound");
        session->layouts.insert(layout);
        auto capture       = std::make_unique<Capture>();
        capture->session   = session;
        capture->directory = session->directory / fmt::format("draw-{:04}", session->captures++);
        std::filesystem::create_directory(capture->directory);
        capture->graphics = ctx.r3.u32;
        capture->cursor   = word(state.data() + 48);
        capture->metadata = toml::table{
            { "schema", 1 }, { "caller", static_cast<std::int64_t>(ctx.lr) }, { "graphics", ctx.r3.u32 }, { "primitive", ctx.r4.u32 }, { "minimum_vertex", ctx.r5.u32 }, { "vertex_count", ctx.r6.u32 }, { "index_count", ctx.r7.u32 }, { "index_address", ctx.r8.u32 }, { "index_format", ctx.r9.u32 }, { "vertex_address", ctx.r10.u32 }, { "stride", stride }, { "cursor_before", capture->cursor }, { "original_returned", false }
        };
        save(*capture, "state-before.be.bin", state);
        const auto vertexSize = std::uint64_t(ctx.r6.u32) * stride;
        const auto indexSize  = std::uint64_t(ctx.r7.u32) * ((ctx.r9.u32 & 4) ? 4 : 2);
        if (!vertexSize || vertexSize > kMaxGeometryBytes || !indexSize || indexSize > kMaxGeometryBytes)
            throw std::runtime_error("guest menu geometry exceeds capture bounds");
        capture->vertexSize = static_cast<std::uint32_t>(vertexSize);
        capture->indexSize  = static_cast<std::uint32_t>(indexSize);
        save(*capture, "vertex.bin", copyMemory(std::uint64_t(ctx.r10.u32) + std::uint64_t(ctx.r5.u32) * stride, vertexSize));
        save(*capture, "index.bin", copyMemory(ctx.r8.u32, indexSize));
        NativeTextureMemoryRange range;
        NativeDrawReplayTexture  texture;
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
        return capture;
    }
    catch (const std::exception& exception)
    {
        fail(exception);
    }
    return {};
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
        if (indexOutput && ctx.r3.u32 == indexOutput && cursor == word(state.data() + 0x3484))
        {
            save(capture, "vertex-output.bin", copyMemory(vertexOutput, capture.vertexSize));
            save(capture, "index-output.bin", copyMemory(indexOutput, capture.indexSize));
        }
        if (cursor > capture.cursor && cursor - capture.cursor <= 256U * 1024U)
            save(capture, "commands.be.bin", copyMemory(capture.cursor, cursor - capture.cursor));
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

bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error)
{
    std::lock_guard lock(captureMutex);
    try
    {
        if (session || !std::filesystem::create_directories(directory))
            throw std::runtime_error("guest draw capture requires a fresh directory and one session");
        session            = std::make_shared<Session>();
        session->directory = directory;
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

void StopNativeGuestDrawCapture()
{
    std::lock_guard lock(captureMutex);
    enabled.store(false, std::memory_order_release);
    if (session && !session->failed && (session->calls < 256 || session->finished != session->captures))
    {
        const std::runtime_error cancelled("guest draw capture stopped before completion");
        fail(cancelled);
    }
    session.reset();
}

} // namespace rerevved::gpu::diagnostics

REX_HOOK_RAW(sub_826A3568)
{
    auto capture = rerevved::gpu::diagnostics::before(ctx);
    __imp__sub_826A3568(ctx, base);
    if (capture)
        rerevved::gpu::diagnostics::after(*capture, ctx);
}
