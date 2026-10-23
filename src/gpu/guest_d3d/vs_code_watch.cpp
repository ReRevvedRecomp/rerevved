#include "vs_code_watch.h"

#include "vs_code_probe.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

REXCVAR_DEFINE_BOOL(native_vs_code_watch,
                    false,
                    "ReRevved",
                    "Observation only, with native_consume_dirty_vertex_fetch: at each consumed dirty vertex fetch draw, compare the VS microcode in guest memory "
                    "with the expected fetch-patched backing words and count vs_watch_consumed, vs_watch_stale, vs_watch_fresh, vs_watch_unreadable, vs_watch_stale_shaders and vs_watch_bytes "
                    "on the native_frame_timing line, with vs_watch_key_cache_stale, vs_watch_key_record_stale and vs_watch_key_unreadable for the patch's key cache "
                    "(G+0x2E38 and the VS variant record); logs the first stale shaders. Does not see guest reads of the words")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

// The big-endian word a four-byte read returned; zero for any other size.
std::uint32_t wordOf(const GuestBytes& bytes)
{
    if (bytes.size() != 4)
        return 0;
    return static_cast<std::uint32_t>(bytes[0]) << 24 | static_cast<std::uint32_t>(bytes[1]) << 16 | static_cast<std::uint32_t>(bytes[2]) << 8 | bytes[3];
}

struct Watch
{
    ReadMemory                 read;
    std::atomic<std::uint64_t> consumed{ 0 }, stale{ 0 }, fresh{ 0 }, unreadable{ 0 }, keyCacheStale{ 0 }, keyRecordStale{ 0 }, keyUnreadable{ 0 }, bytes{ 0 };
    std::mutex                 lock;
    std::vector<std::uint32_t> staleVertexShaders;
};

// The installed probe. A consume in flight holds its own reference.
std::mutex             installLock;
std::shared_ptr<Watch> installed;

void observe(const VertexFetchConsume& consume) noexcept
{
    if (!VsCodeWatchRequested())
        return;
    std::shared_ptr<Watch> watch;
    {
        const std::lock_guard guard(installLock);
        watch = installed;
    }
    if (!watch) [[unlikely]]
        return;
    // Every fallible read comes first, so a draw is counted once, as one outcome.
    std::uint32_t strideBytes = 0, vertex = 0;
    VsCodeReading reading;
    VsKeyReading  keys;
    bool          readable = true;
    try
    {
        if (consume.stride)
        {
            // A wrapper's stream 0 stride: the word in its stack frame, or the register.
            strideBytes = consume.stride->value;
            if (consume.stride->inStack)
            {
                const auto bytes = watch->read(consume.stride->value, 4, false);
                if (bytes.size() != 4)
                    throw std::runtime_error("stride_unreadable");
                strideBytes = wordOf(bytes);
            }
        }
        else
        {
            // The IndexedBuffer entry: the byte G+0x30F0, in words.
            const auto bytes = watch->read(std::uint64_t(consume.graphics) + 0x30F0, 4, false);
            if (bytes.size() != 4)
                throw std::runtime_error("stride_unreadable");
            strideBytes = std::uint32_t(bytes[0]) * 4;
        }
        reading = ReadVsCodeState(consume.graphics, strideBytes, watch->read);
        keys    = ReadVsKeyState(consume.graphics, strideBytes, watch->read);
        if (reading.state == VsCodeState::Stale)
            vertex = wordOf(watch->read(std::uint64_t(consume.graphics) + 0x3198, 4, false));
    }
    catch (const std::exception&)
    {
        readable = false;
    }
    watch->consumed.fetch_add(1, std::memory_order_relaxed);
    if (!readable)
    {
        watch->unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!keys.readable)
        watch->keyUnreadable.fetch_add(1, std::memory_order_relaxed);
    else
    {
        if (keys.cacheDiffers)
            watch->keyCacheStale.fetch_add(1, std::memory_order_relaxed);
        if (keys.recordDiffers)
            watch->keyRecordStale.fetch_add(1, std::memory_order_relaxed);
    }
    switch (reading.state)
    {
        case VsCodeState::Fresh:
            watch->fresh.fetch_add(1, std::memory_order_relaxed);
            break;
        case VsCodeState::Unreadable:
            watch->unreadable.fetch_add(1, std::memory_order_relaxed);
            break;
        case VsCodeState::Stale:
        {
            watch->stale.fetch_add(1, std::memory_order_relaxed);
            try
            {
                const std::lock_guard guard(watch->lock);
                if (std::ranges::find(watch->staleVertexShaders, vertex) == watch->staleVertexShaders.end() && watch->staleVertexShaders.size() < kVsWatchShaderCap)
                {
                    watch->staleVertexShaders.push_back(vertex);
                    if (watch->staleVertexShaders.size() <= kVsWatchLogLimit)
                        REXLOG_INFO("Native VS code watch: VS {:08X} holds words {} where the fetch patch expects {} (stride {})", vertex, reading.readDigest.substr(0, 16), reading.hostDigest.substr(0, 16), strideBytes);
                }
            }
            catch (const std::exception&)
            {
                // Only the shader list and its log are lost.
            }
            break;
        }
    }
}

} // namespace

bool VsCodeWatchRequested() noexcept
{
    return REXCVAR_GET(native_vs_code_watch);
}

void InstallVsCodeWatch(ReadMemory read)
{
    if (!VsCodeWatchRequested())
        return;
    auto watch = std::make_shared<Watch>();
    // Diagnostic validation must use the caller's guarded copy, never a raw view.
    read.view = {};
    read.shaderCache.reset();
    watch->read = [read = std::move(read), counters = watch.get()](std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto bytes = read(address, size, physical);
        counters->bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
        return bytes;
    };
    {
        const std::lock_guard guard(installLock);
        installed = std::move(watch);
    }
    SetVertexFetchConsumeObserver(&observe);
}

void RemoveVsCodeWatch() noexcept
{
    SetVertexFetchConsumeObserver(nullptr);
    const std::lock_guard guard(installLock);
    installed.reset();
}

VsCodeWatchCounts TakeVsCodeWatchCounts() noexcept
{
    VsCodeWatchCounts      counts;
    std::shared_ptr<Watch> watch;
    {
        const std::lock_guard guard(installLock);
        watch = installed;
    }
    if (!watch)
        return counts;
    counts.consumed       = watch->consumed.exchange(0, std::memory_order_relaxed);
    counts.stale          = watch->stale.exchange(0, std::memory_order_relaxed);
    counts.fresh          = watch->fresh.exchange(0, std::memory_order_relaxed);
    counts.unreadable     = watch->unreadable.exchange(0, std::memory_order_relaxed);
    counts.keyCacheStale  = watch->keyCacheStale.exchange(0, std::memory_order_relaxed);
    counts.keyRecordStale = watch->keyRecordStale.exchange(0, std::memory_order_relaxed);
    counts.keyUnreadable  = watch->keyUnreadable.exchange(0, std::memory_order_relaxed);
    counts.bytes          = watch->bytes.exchange(0, std::memory_order_relaxed);
    const std::lock_guard guard(watch->lock);
    counts.staleShaders = watch->staleVertexShaders.size();
    return counts;
}

std::string FormatVsCodeWatchCounts(const VsCodeWatchCounts& counts)
{
    return std::format(" vs_watch_consumed={} vs_watch_stale={} vs_watch_fresh={} vs_watch_unreadable={} vs_watch_stale_shaders={} vs_watch_key_cache_stale={} vs_watch_key_record_stale={} vs_watch_key_unreadable={} vs_watch_bytes={}",
                       counts.consumed,
                       counts.stale,
                       counts.fresh,
                       counts.unreadable,
                       counts.staleShaders,
                       counts.keyCacheStale,
                       counts.keyRecordStale,
                       counts.keyUnreadable,
                       counts.bytes);
}

} // namespace rerevved::gpu::guest_d3d
