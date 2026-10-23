#include "guest_progress_hooks.h"

#include "completion_ledger.h"
#include "gpu/guest_d3d/clear_consume.h"
#include "gpu/guest_d3d/clear_derive.h"
#include "gpu/guest_d3d/guest_render_capture.h"
#include "gpu/guest_d3d/native_draw_dispatch.h"
#include "gpu/guest_d3d/resolve_consume.h"
#include "gpu/guest_d3d/resolve_derive.h"
#include "gpu/guest_d3d/ring_stubs.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::completion
{

std::atomic<bool> guestProgressActive{ false };

namespace
{

std::mutex                                 publicationMutex;
std::shared_ptr<CompletionLedger>          published;
std::weak_ptr<guest_d3d::IGuestRenderSink> publishedAssembler;

struct ResolveDraft
{
    guest_d3d::GuestResolveCopy copy;
    guest_d3d::GuestTarget      target;
    std::uint32_t               g = 0, flags = 0;
    bool                        valid = false;
    bool                        open  = false; // the ledger awaits this copy hook
    std::string                 detail;        // why the source set did not decode
};

thread_local ResolveDraft resolveDraft;

// Diagnostics for the first tiled scanouts: the per-tile copies as the guest
// issued them, and the logical clear that the resolve-time clear should equal.
struct TiledScanoutLog
{
    std::uint32_t received = 0, depthClear = 0, colorClear = 0;
    std::string   rects, points, controls, dest;
};

thread_local TiledScanoutLog                     tiledScanoutLog;
thread_local std::optional<std::array<float, 4>> logicalClear;
std::atomic<std::uint32_t>                       tiledScanoutsLogged{ 0 };
constexpr std::uint32_t                          kTiledScanoutLogLimit = 4;

void logTiledScanout(const guest_d3d::GuestTiling& tiling, const guest_d3d::GuestResolveCopy& copy)
{
    auto& log = tiledScanoutLog;
    // Tile 0 starts a set, so an incomplete earlier one is dropped.
    if (copy.rectX == tiling.rects[0].x0 && copy.rectY == tiling.rects[0].y0)
        log = {};
    const auto separator = log.received ? ";" : "";
    log.rects += std::format("{}{},{},{},{}", separator, copy.rectX, copy.rectY, copy.rectX + copy.rectWidth, copy.rectY + copy.rectHeight);
    log.points += std::format("{}{},{}", separator, copy.destX, copy.destY);
    log.controls += std::format("{}{:08X}", log.received ? "," : "", copy.copyControl);
    log.dest += std::format("{}{:08X}", log.received ? "," : "", copy.destBase);
    if (!log.received)
    {
        log.depthClear = copy.depthClear;
        log.colorClear = copy.colorClear;
    }
    if (++log.received < tiling.count)
        return;
    if (tiledScanoutsLogged.fetch_add(1) < kTiledScanoutLogLimit)
    {
        const auto clear = logicalClear ? std::format("{},{},{},{}", (*logicalClear)[0], (*logicalClear)[1], (*logicalClear)[2], (*logicalClear)[3]) : std::string("none");
        REXGPU_INFO("Native scanout: tiles={} rects={} points={} controls={} dest={} resolve_clear={:08X},{:08X} logical_clear={}",
                    tiling.count,
                    log.rects,
                    log.points,
                    log.controls,
                    log.dest,
                    log.depthClear,
                    log.colorClear,
                    clear);
    }
    log = {};
}

std::shared_ptr<CompletionLedger> currentLedger()
{
    if (!guestProgressActive.load(std::memory_order_acquire))
        return {};
    std::lock_guard lock(publicationMutex);
    return published;
}

std::shared_ptr<guest_d3d::IGuestRenderSink> currentAssembler()
{
    std::lock_guard lock(publicationMutex);
    return publishedAssembler.lock();
}

rex::memory::Memory* guestMemory()
{
    auto* kernel = rex::system::kernel_state();
    return kernel ? kernel->memory() : nullptr;
}

bool readable(rex::memory::Memory* memory, std::uint32_t address, std::uint32_t size)
{
    if (!memory || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
        return false;
    const auto last = address + size - 1;
    auto*      heap = memory->LookupHeap(address);
    return heap && memory->LookupHeap(last) == heap &&
           (static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly));
}

// The guest reads the hooks have attempted on this thread (GuestProgressReadCount).
thread_local std::uint64_t guestReads = 0;

bool readWord(std::uint32_t address, std::uint32_t& value)
{
    ++guestReads;
    auto* memory = guestMemory();
    if (!readable(memory, address, 4))
        return false;
    std::uint32_t raw = 0;
    std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
    value = rex::byte_swap(raw);
    return true;
}

bool readByte(std::uint32_t address, std::uint8_t& value)
{
    ++guestReads;
    auto* memory = guestMemory();
    if (!readable(memory, address, 1))
        return false;
    value = *memory->TranslateVirtual<const std::uint8_t*>(address);
    return true;
}

// True when every byte of [address, address + size) is writable guest memory.
bool writable(rex::memory::Memory* memory, std::uint32_t address, std::uint32_t size)
{
    if (!memory || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
        return false;
    const auto last   = address + size - 1;
    auto*      heap   = memory->LookupHeap(address);
    const auto needed = static_cast<unsigned>(rex::memory::PageAccess::kReadWrite);
    return heap && memory->LookupHeap(last) == heap && (static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & needed) == needed;
}

// The big-endian word or byte store a guest `stw` or `stb` makes.
bool writeGuest(const guest_d3d::ResolveWrite& write)
{
    auto* memory = guestMemory();
    if (!writable(memory, write.address, write.byte ? 1 : 4))
        return false;
    if (write.byte)
    {
        *memory->TranslateVirtual<std::uint8_t*>(write.address) = static_cast<std::uint8_t>(write.value);
        return true;
    }
    const std::uint32_t raw = rex::byte_swap(write.value);
    std::memcpy(memory->TranslateVirtual<std::uint8_t*>(write.address), &raw, sizeof(raw));
    return true;
}

std::optional<std::uint32_t> physicalAddress(std::uint32_t address)
{
    auto* memory = guestMemory();
    if (!readable(memory, address, 4))
        return std::nullopt;
    const auto physical = memory->GetPhysicalAddress(address);
    if (physical == UINT32_MAX)
        return std::nullopt;
    return physical;
}

// The guest's own translation of a command buffer address, as 0x826AC3B8
// computes it for the kick: the 0xE0000000 view is offset by 0x1000.
std::uint32_t guestPhysical(std::uint32_t address)
{
    return (address & 0x1FFFFFFFU) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

// The device G through the global 0x820009F0, as 0x8269BF40 reads it.
std::optional<std::uint32_t> globalDevice()
{
    std::uint32_t global = 0, device = 0;
    if (!readWord(0x820009F0, global) || !readWord(global, device))
        return std::nullopt;
    return device;
}

// G+0x33B0 is set by BeginTiling 0x826AC0A8 and cleared by 0x826AC218. Segments
// closed meanwhile are replayed once per tile by the worker, after the
// EndTiling callback starts it, so progress packets in them have no model.
std::optional<bool> tiling(std::uint32_t g)
{
    std::uint32_t value = 0;
    if (g > 0xFFFFFFFFU - 0x33B0 || !readWord(g + 0x33B0, value))
        return std::nullopt;
    return value != 0;
}

// True when the record was refused as tiled or the tiling state is unreadable.
bool refuseTiled(CompletionLedger& ledger, std::uint32_t g, ProgressKind kind, std::uint32_t site)
{
    const auto active = tiling(g);
    if (!active)
        ledger.Fail("tiling_state_unreadable", kind, site);
    else if (*active)
        ledger.TiledProgress(kind, site);
    return !active || *active;
}

// 0x8269BCA8 is a leaf that loads every value it emits at entry: B from
// G+0x2A90, the fence from G+0x2A9C and the cursor from G+0x30 | (G+0x3A48 & 3).
void observeFence(CompletionLedger& ledger, const PPCContext& ctx)
{
    constexpr std::uint32_t kEmitter = 0x8269BCA8;
    const std::uint32_t     g        = ctx.r3.u32;
    std::uint32_t           block = 0, fence = 0, position = 0, low = 0, cpuFenceOff = 0;
    std::uint8_t            mode = 0;
    if (g > 0xFFFFFFFFU - 0x5420 || !readWord(g + 0x2A90, block) || !readWord(g + 0x2A9C, fence) || !readWord(g + 0x30, position) ||
        !readWord(g + 0x3A48, low) || !readByte(g + 0x2ABD, mode) || !readWord(g + 0x541C, cpuFenceOff))
    {
        ledger.Fail("fence_emitter_state_unreadable", ProgressKind::Fence, kEmitter);
        return;
    }
    if (refuseTiled(ledger, g, ProgressKind::Fence, kEmitter))
        return;
    // With G+0x2ABD bit 0x2 set (by the guest in 0x8269C588) the emitter also
    // stores B+0 and B+4 on the CPU; that completion is not GPU progress.
    if ((mode & 2) && cpuFenceOff == 0)
        ledger.Fail("cpu_fence_mode: G+0x2ABD bit 0x2", ProgressKind::Fence, kEmitter);
    const auto physicalBlock = physicalAddress(block);
    if (!physicalBlock)
    {
        ledger.Fail(std::format("fence_block_unmapped {:#010x}", block), ProgressKind::Fence, kEmitter);
        return;
    }
    // 0x826AC218 emits this fence at 0x826AC33C into the side buffer r4 + 4,
    // which it submits through 0x8269BD98 after the worker-kick callback
    // buffer; the ledger holds both of its records for that buffer.
    if (static_cast<std::uint32_t>(ctx.lr) == 0x826AC340)
    {
        ledger.Fence(*physicalBlock, fence, position | (low & 3), guestPhysical(ctx.r4.u32 + 4));
        return;
    }
    ledger.InvalidateState(kEmitter);
    ledger.Fence(*physicalBlock, fence, position | (low & 3));
}

// 0x8269BF40 reads S2 through the device global at 0x820009F0 and ORs the
// predicate at G+0x31AC into its wait and interrupt packets; G+0x31A4 is the
// bin mask that predicate tests.
void observeCallback(CompletionLedger& ledger, const PPCContext& ctx)
{
    constexpr std::uint32_t kEmitter = 0x8269BF40;
    std::uint32_t           global = 0, device = 0, scratch = 0, predicate = 0, predicateMask = 0;
    if (!readWord(0x820009F0, global) || !readWord(global, device) || device > 0xFFFFFFFFU - 0x2A94 ||
        !readWord(device + 0x2A94, scratch) || ctx.r3.u32 > 0xFFFFFFFFU - 0x31AC || !readWord(ctx.r3.u32 + 0x31AC, predicate) ||
        (predicate != 0 && !readWord(ctx.r3.u32 + 0x31A4, predicateMask)))
    {
        ledger.Fail("callback_emitter_state_unreadable", ProgressKind::Callback, kEmitter);
        return;
    }
    const auto physicalScratch = physicalAddress(scratch);
    if (!physicalScratch)
    {
        ledger.Fail(std::format("scratch_block_unmapped {:#010x}", scratch), ProgressKind::Callback, kEmitter);
        return;
    }
    // Only a first-pass predicate keeps a tiled callback to one interrupt.
    if (predicate == 0 && refuseTiled(ledger, ctx.r3.u32, ProgressKind::Callback, kEmitter))
        return;
    // 0x826AC218 emits its worker-kick callback at 0x826AC32C into the side
    // buffer r4 + 4 and submits that buffer at 0x826AC3B0.
    const auto sideBuffer = static_cast<std::uint32_t>(ctx.lr) == 0x826AC330 ? std::optional<std::uint32_t>{ guestPhysical(ctx.r4.u32 + 4) } : std::nullopt;
    ledger.Callback(*physicalScratch, ctx.r6.u32, ctx.r7.u32, ctx.r5.u32, predicate != 0 ? std::optional<std::uint32_t>{ predicateMask } : std::nullopt, sideBuffer);
}

// The guest's fetch base is a virtual address in a physical-view heap
// (0xFE3AF000 for 0x1E3B0000); GetPhysicalAddress applies the 0xE0000000
// view's 0x1000 offset. A base already below 0x20000000 is physical.
std::optional<std::uint32_t> physicalBase(std::uint32_t base)
{
    auto* memory = guestMemory();
    if (!memory)
        return std::nullopt;
    const auto physical = memory->GetPhysicalAddress(base);
    if (physical != UINT32_MAX)
        return physical;
    if (base <= 0x1FFFFFFFU)
        return base;
    return std::nullopt;
}

// Resolve probes stack entry observations through return 0x8269F360, retaining
// derived copy and clear words.
thread_local guest_d3d::ResolveProbeStack resolveProbes;
#if REREVVED_DEVELOPER
std::atomic<std::uint32_t> resolveDeriveLogged{ 0 }, resolveDeriveUnsupportedLogged{ 0 }, resolveClearStaleLogged{ 0 };
std::atomic<std::uint32_t> resolveEffectLogged{ 0 }, resolveEffectUnsupportedLogged{ 0 };
constexpr std::uint32_t    kResolveDeriveLogLimit = 8, kResolveClearStaleLogLimit = 4;
#endif

// Unadmitted Resolve calls get placeholder probes so nested hooks cannot attach
// to an outer call.
void beginResolveProbe(const PPCContext& ctx, bool admitted)
{
    guest_d3d::ResolveProbe probe;
    probe.admitted   = admitted;
    probe.deriveCopy = guest_d3d::ResolveDeriveCheckRequested();
    probe.effects    = admitted && guest_d3d::ResolveEffectCheckRequested();
#if REREVVED_DEVELOPER
    probe.g = ctx.r3.u32;
    if (admitted)
    {
        probe.entry = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
        // Before any guest word of the probe is read: the skipped Original of the consume the exit model describes
        // would store where the model reads.
        if (probe.effects)
            probe.stackOverlap = guest_d3d::ResolveStackOverlaps(readWord, { ctx.r13.u32, ctx.r1.u32 }, probe.entry);
        if (probe.deriveCopy)
            probe.derived = guest_d3d::DeriveResolveWords(readWord, probe.entry, &probe.why);
        if (probe.effects && !probe.stackOverlap)
        {
            const guest_d3d::ResolveClearArgs clear{ ctx.r1.u32, ctx.f1.f64 };
            probe.predicted = guest_d3d::DeriveResolveEffects(readWord, probe.entry, &probe.effectsWhy, &clear);
            if (probe.predicted && !guest_d3d::ReadResolveWatch(readWord, probe.g, probe.entry[3], probe.entryWatch))
            {
                probe.predicted.reset();
                probe.effectsWhy = "unreadable watched words at entry";
            }
        }
    }
    guest_d3d::ResolveDropCounts drops;
    resolveProbes.Push(ctx.r1.u32, std::move(probe), &drops);
    guest_d3d::NoteResolveUnpaired(drops.derive);
    guest_d3d::NoteResolveEffectUnpaired(drops.effects);
#else
    resolveProbes.Push(ctx.r1.u32, std::move(probe));
#endif
}

#if REREVVED_DEVELOPER
// At 0x8269F360: reads the watched words again and compares them with the
// entry reading and the prediction.
void compareResolveEffects(const guest_d3d::ResolveProbe& probe)
{
    const auto& r = probe.entry;
    // Counted before the watched words are read, so an unreadable one cannot mask it.
    if (probe.stackOverlap)
    {
        guest_d3d::NoteResolveEffectStackOverlap();
        return;
    }
    if (!probe.predicted)
    {
        guest_d3d::NoteResolveEffectUnsupported();
        if (resolveEffectUnsupportedLogged.fetch_add(1) < kResolveDeriveLogLimit)
            REXGPU_INFO("Native resolve effect unsupported: {} entry r3..r10={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}", probe.effectsWhy, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        return;
    }
    guest_d3d::ResolveWatch exit;
    if (!guest_d3d::ReadResolveWatch(readWord, probe.g, r[3], exit))
    {
        guest_d3d::NoteResolveEffectUnsupported();
        if (resolveEffectUnsupportedLogged.fetch_add(1) < kResolveDeriveLogLimit)
            REXGPU_INFO("Native resolve effect unsupported: unreadable watched words at return entry r3..r10={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        return;
    }
    const auto result = guest_d3d::CompareResolveEffects(*probe.predicted, probe.entryWatch, exit);
    guest_d3d::NoteResolveEffect(result);
    if (!result.mismatch || resolveEffectLogged.fetch_add(1) >= kResolveDeriveLogLimit)
        return;
    using Field    = guest_d3d::ResolveEffectField;
    const auto has = [&](Field field)
    {
        return (result.mismatch & (1U << static_cast<unsigned>(field))) != 0;
    };
    std::string fields;
    if (has(Field::CopyWords))
        fields += std::format(" copy_words=predicted {:08X},{:08X},{:08X},{:08X} actual {:08X},{:08X},{:08X},{:08X}",
                              result.expectCopy[0],
                              result.expectCopy[1],
                              result.expectCopy[2],
                              result.expectCopy[3],
                              exit.copy[0],
                              exit.copy[1],
                              exit.copy[2],
                              exit.copy[3]);
    if (has(Field::Size))
        fields += std::format(" size=predicted {:08X} actual {:08X}", result.expectSize, exit.size);
    if (has(Field::DirtySetMissing))
        fields += std::format(" dirty_set_missing=G+10 {:016X} G+18 {:016X} G+20 {:016X}", result.missing[0], result.missing[1], result.missing[2]);
    if (has(Field::DirtyExtra))
        fields += std::format(" dirty_extra=G+10 {:016X} G+18 {:016X} G+20 {:016X}", result.extra[0], result.extra[1], result.extra[2]);
    if (has(Field::Bytes))
        fields += std::format(" bytes=G+2ABD {:02X}->{:02X} G+2ABE {:02X}->{:02X}", probe.entryWatch.allocByte, exit.allocByte, probe.entryWatch.stateByte, exit.stateByte);
    if (has(Field::Dest))
        fields += std::format(" dest=fence entry {:08X} return {:08X} actual +8 {:08X} +12 {:08X} (entry +8 {:08X} +12 {:08X})", probe.entryWatch.fence, exit.fence, exit.dest8, exit.dest12, probe.entryWatch.dest8, probe.entryWatch.dest12);
    REXGPU_WARN("Native resolve effect mismatch:{} main_alloc_failed={} entry dirty G+10 {:016X} G+18 {:016X} G+20 {:016X} exit dirty {:016X} {:016X} {:016X} entry r3..r10={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
                fields,
                result.mainFailed,
                probe.entryWatch.dirty[0],
                probe.entryWatch.dirty[1],
                probe.entryWatch.dirty[2],
                exit.dirty[0],
                exit.dirty[1],
                exit.dirty[2],
                r[0],
                r[1],
                r[2],
                r[3],
                r[4],
                r[5],
                r[6],
                r[7]);
}

// At 0x8269E8A0 with the body's r1 `frame`: compares the words the guest stored
// with the derivation and keeps the clear words, which the body stores only
// after this hook.
void compareResolveDerive(std::uint32_t g, std::uint32_t frame)
{
    unsigned                     dropped = 0;
    guest_d3d::ResolveDropCounts drops;
    auto*                        found = resolveProbes.Find(frame, dropped, &drops);
    guest_d3d::NoteResolveUnpaired(drops.derive);
    guest_d3d::NoteResolveEffectUnpaired(drops.effects);
    if (!found || !found->admitted || !found->deriveCopy)
        return;
    auto& probe = *found;
    if (probe.g != g)
    {
        probe.admitted = false;
        guest_d3d::NoteResolveUnpaired(1);
        return;
    }
    guest_d3d::GuestResolveCopy copy;
    if (!guest_d3d::ReadGuestResolveCopy(readWord, g, copy))
    {
        guest_d3d::NoteResolveDerive(std::nullopt, {});
        probe.admitted = false;
        return;
    }
    probe.hookClear = { copy.depthClear, copy.colorClear, copy.colorClearLow };
    probe.hookRead  = true;
    const guest_d3d::ResolveCopyWords observed{ copy.copyControl, copy.destBase, copy.destPitch, copy.destInfo };
    const auto                        mask = guest_d3d::NoteResolveDerive(probe.derived, observed);
    const auto&                       r    = probe.entry;
    if (!probe.derived)
    {
        if (resolveDeriveUnsupportedLogged.fetch_add(1) < kResolveDeriveLogLimit)
            REXGPU_INFO("Native resolve derive unsupported: {} entry r3..r10={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}", probe.why, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        return;
    }
    if (!mask || resolveDeriveLogged.fetch_add(1) >= kResolveDeriveLogLimit)
        return;
    std::string                                                                             words;
    const auto                                                                              derived = probe.derived;
    const std::array<std::pair<std::uint32_t, std::uint32_t>, guest_d3d::kResolveWordCount> pairs{ { { derived->copyControl, observed.copyControl },
                                                                                                     { derived->destBase, observed.destBase },
                                                                                                     { derived->destPitch, observed.destPitch },
                                                                                                     { derived->destInfo, observed.destInfo } } };
    for (std::size_t i = 0; i < pairs.size(); ++i)
        if (mask & (1U << i))
            words += std::format(" {}=derived {:08X} guest {:08X}", guest_d3d::kResolveWordNames[i], pairs[i].first, pairs[i].second);
    REXGPU_WARN("Native resolve derive mismatch:{} entry r3..r10={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}", words, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}
#endif

// Clear probes use entry r1-368 as body frame for per-target hook 0x826A1D90
// and exit 0x826A2960 of body 0x826A22F8; both hooks find their call by frame.
thread_local guest_d3d::ClearProbeStack clearProbes;
#if REREVVED_DEVELOPER
std::atomic<std::uint32_t> clearDeriveLogged{ 0 }, clearDeriveUnsupportedLogged{ 0 };
std::atomic<std::uint32_t> clearEffectLogged{ 0 }, clearEffectUnsupportedLogged{ 0 }, clearExitLogged{ 0 };
constexpr std::uint32_t    kClearDeriveLogLimit = 8;
#endif

#if REREVVED_DEVELOPER
// The exact exit state of the consume model for a probe with its prediction and
// entry reading, or the reason there is none.
void deriveClearProbeExit(guest_d3d::ClearProbe& probe, const guest_d3d::ClearCalls& calls)
{
    const auto&                 effects = *probe.predicted;
    guest_d3d::ClearSideBuffers sides;
    if (effects.reachedFlush)
    {
        auto derived = guest_d3d::DeriveClearSideBuffers(readWord, probe.entry, calls);
        if (!derived)
        {
            probe.exitRefusal = guest_d3d::ClearExitRefusal::AllocPath;
            return;
        }
        sides           = *derived;
        probe.sideTakes = sides.count;
    }
    probe.exit = guest_d3d::DeriveClearExitState(effects, sides, probe.entryWatch, &probe.exitRefusal);
}

// Derives at entry, before Original runs, and stacks the call.
void beginClearDeriveProbe(const PPCContext& ctx)
{
    guest_d3d::ClearProbe probe;
    probe.deriveCalls = guest_d3d::ClearDeriveCheckRequested();
    probe.effects     = guest_d3d::ClearEffectCheckRequested();
    probe.entry       = { ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, ctx.r8.u32, ctx.r13.u32 };
    // Before any guest word of the probe is read: the skipped Original of the consume the exit model
    // describes would store where the model reads. An overlap ends the effects probe there: it reads no
    // further word (an unreadable one could not mask it) and the exit counts it alone.
    if (probe.effects)
        probe.stackOverlap = guest_d3d::ClearStackOverlaps(readWord, probe.entry);
    const bool effects = probe.effects && !probe.stackOverlap;
    auto       calls   = (probe.deriveCalls || effects) ? guest_d3d::DeriveClearCalls(readWord, probe.entry, &probe.why) : std::nullopt;
    if (effects)
    {
        if (!calls)
            probe.effectsWhy = probe.why;
        else
        {
            probe.predicted = guest_d3d::DeriveClearEffects(readWord, probe.entry, *calls, &probe.effectsWhy);
            if (probe.predicted && !guest_d3d::ReadClearWatch(readWord, probe.entry.g, probe.entryWatch))
            {
                probe.predicted.reset();
                probe.effectsWhy = "unreadable watched words at entry";
            }
            if (probe.predicted)
                deriveClearProbeExit(probe, *calls);
        }
    }
    if (probe.deriveCalls)
        probe.derived = std::move(calls);
    guest_d3d::ClearDropCounts drops;
    clearProbes.Push(ctx.r1.u32, std::move(probe), &drops);
    guest_d3d::NoteClearUnpaired(drops.derive);
    guest_d3d::NoteClearEffectUnpaired(drops.effects);
}
#endif

// Published ring services create a placeholder Clear probe; Original and
// consumed exits close their own probe.
void stackClearPlaceholder(const PPCContext& ctx)
{
    guest_d3d::ClearProbe probe;
    probe.deriveCalls = false;
    probe.effects     = false;
#if REREVVED_DEVELOPER
    probe.entry = { ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, ctx.r8.u32, ctx.r13.u32 };
#endif
    clearProbes.Push(ctx.r1.u32, std::move(probe));
}

#if REREVVED_DEVELOPER
// At the hook on 0x826A1D90 with the body's r1 `stack`: records the call the
// guest made. A device other than the probe's drops the probe.
void observeClearDerive(const std::array<std::uint32_t, 8>& arguments, std::uint32_t stack, double depth)
{
    unsigned                   dropped = 0;
    guest_d3d::ClearDropCounts drops;
    auto*                      found = clearProbes.Find(stack, dropped, &drops);
    guest_d3d::NoteClearUnpaired(drops.derive);
    guest_d3d::NoteClearEffectUnpaired(drops.effects);
    if (!found || !found->live)
        return;
    if (found->entry.g != arguments[0])
    {
        found->live = false;
        guest_d3d::NoteClearUnpaired(found->deriveCalls ? 1 : 0);
        guest_d3d::NoteClearEffectUnpaired(found->effects ? 1 : 0);
        return;
    }
    if (!found->deriveCalls)
        return;
    guest_d3d::ClearCall call;
    std::string          reason;
    if (!guest_d3d::ObserveClearCall(readWord, arguments, stack, depth, call, &reason))
    {
        found->observedUnreadable = true;
        if (found->why.empty())
            found->why = std::move(reason);
    }
    found->observed.Add(call);
}
#else
// At the hook on 0x826A1D90 with the body's r1 `stack`: drops the probes stacked
// above this call's, as a hook that finds its call by frame does.
void observeClearDerive(const std::array<std::uint32_t, 8>&, std::uint32_t stack, double)
{
    unsigned dropped = 0;
    clearProbes.Find(stack, dropped);
}
#endif

#if REREVVED_DEVELOPER
// At the exit of the body: compares the derived list with the recorded one.
void compareClearDerive(const guest_d3d::ClearProbe& probe)
{
    const auto  mask = guest_d3d::NoteClearDerive(probe.derived, probe.observed, !probe.observedUnreadable);
    const auto& e    = probe.entry;
    if (!probe.derived || probe.observedUnreadable)
    {
        if (clearDeriveUnsupportedLogged.fetch_add(1) < kClearDeriveLogLimit)
            REXGPU_INFO("Native clear derive unsupported: {} entry r1={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r8={:08X} f1={:016X} observed={}",
                        probe.why,
                        e.sp,
                        e.g,
                        e.flags,
                        e.rect,
                        e.color,
                        e.stencil,
                        std::bit_cast<std::uint64_t>(e.depth),
                        guest_d3d::FormatClearCalls(probe.observed));
        return;
    }
    if (!mask || clearDeriveLogged.fetch_add(1) >= kClearDeriveLogLimit)
        return;
    std::string fields;
    for (std::size_t i = 0; i < guest_d3d::kClearFieldCount; ++i)
        if (mask & (1U << i))
            fields += std::format(" {}", guest_d3d::kClearFieldNames[i]);
    REXGPU_WARN("Native clear derive mismatch:{} entry r1={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r8={:08X} f1={:016X} derived={} observed={}",
                fields,
                e.sp,
                e.g,
                e.flags,
                e.rect,
                e.color,
                e.stencil,
                std::bit_cast<std::uint64_t>(e.depth),
                guest_d3d::FormatClearCalls(*probe.derived),
                guest_d3d::FormatClearCalls(probe.observed));
}

// At the exit of the body: reads the watched words again and compares them with
// the entry reading and the prediction.
void compareClearEffects(const guest_d3d::ClearProbe& probe)
{
    const auto& e           = probe.entry;
    const auto  unsupported = [&](const std::string& reason)
    {
        guest_d3d::NoteClearEffectUnsupported();
        if (clearEffectUnsupportedLogged.fetch_add(1) < kClearDeriveLogLimit)
            REXGPU_INFO("Native clear effect unsupported: {} entry r1={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r8={:08X} f1={:016X}",
                        reason,
                        e.sp,
                        e.g,
                        e.flags,
                        e.rect,
                        e.color,
                        e.stencil,
                        std::bit_cast<std::uint64_t>(e.depth));
    };
    // Counted before any guest word is read, so an unreadable one cannot mask it.
    if (probe.stackOverlap)
    {
        guest_d3d::NoteClearExitUnmodeled(guest_d3d::ClearExitRefusal::StackOverlap);
        return;
    }
    if (!probe.predicted)
    {
        unsupported(probe.effectsWhy);
        return;
    }
    guest_d3d::ClearWatch exit;
    if (!guest_d3d::ReadClearWatch(readWord, e.g, exit))
    {
        unsupported("unreadable watched words at exit");
        return;
    }
    const auto result = guest_d3d::CompareClearEffects(*probe.predicted, probe.entryWatch, exit);
    guest_d3d::NoteClearEffect(*probe.predicted, result);
    // The exit state the consume would write, against the real exit.
    if (probe.exit)
    {
        const auto mask = guest_d3d::ClearExitMismatches(*probe.exit, exit);
        guest_d3d::NoteClearExit(mask, probe.exit->shape);
        if (mask && clearExitLogged.fetch_add(1) < kClearDeriveLogLimit)
        {
            const auto& x = *probe.exit;
            REXGPU_WARN("Native clear exit mismatch mask={:02X}: predicted dirty {:016X} {:016X} {:016X} G+2ABD {:02X} G+34 {:08X} G+38 {:08X} G+3A44 {:08X} G+3A54 {:08X} G+3A4C {:08X} actual dirty {:016X} {:016X} {:016X} G+2ABD {:02X} G+34 {:08X} G+38 {:08X} G+3A44 {:08X} G+3A54 {:08X} G+3A4C {:08X} entry G+30 {:08X} G+34 {:08X} G+38 {:08X} G+3A44 {:08X} G+3A54 {:08X} G+3A58 {:08X} G+3A4C {:08X} n={} pool={:08X}+{:08X} +{:08X} null area G+3A3C {:08X} G+3A50 {:08X} G+3A48 {:08X} G+33B4 {:08X} progress {:08X} calls={} r1={:08X} r3={:08X} shape={:02X} tile_byte={:02X} bracket={:08X} predicate={:08X} loop_tiles={} takes={}",
                        mask,
                        x.dirty[0],
                        x.dirty[1],
                        x.dirty[2],
                        x.allocByte,
                        x.limit34,
                        x.limit38,
                        x.poolPos,
                        x.poolUsed,
                        x.wrapPos,
                        exit.dirty[0],
                        exit.dirty[1],
                        exit.dirty[2],
                        exit.allocByte,
                        exit.limit34,
                        exit.limit38,
                        exit.cursor,
                        exit.allocated,
                        exit.wrapPos,
                        probe.entryWatch.ring,
                        probe.entryWatch.limit34,
                        probe.entryWatch.limit38,
                        probe.entryWatch.cursor,
                        probe.entryWatch.allocated,
                        probe.entryWatch.poolThreshold,
                        probe.entryWatch.wrapPos,
                        probe.entryWatch.targetCount,
                        probe.entryWatch.poolObject,
                        probe.entryWatch.poolBase,
                        probe.entryWatch.poolSize,
                        probe.entryWatch.sideEnd,
                        probe.entryWatch.wrapLimit,
                        probe.entryWatch.segment,
                        probe.entryWatch.bracketNext,
                        probe.entryWatch.progress,
                        probe.predicted->calls,
                        e.sp,
                        e.g,
                        x.shape,
                        probe.entryWatch.tilingByte,
                        probe.entryWatch.bracket,
                        probe.entryWatch.predicate,
                        probe.predicted->loopTiles,
                        probe.sideTakes);
        }
    }
    else
        guest_d3d::NoteClearExitUnmodeled(probe.exitRefusal);
    if (!result.mismatch || clearEffectLogged.fetch_add(1) >= kClearDeriveLogLimit)
        return;
    std::string words;
    for (std::size_t i = 0; i < guest_d3d::kClearDirtyWords; ++i)
        if (result.mismatch & (1U << i))
            words += std::format(" {}=predicted {:016X} actual {:016X} xor {:016X}", guest_d3d::kClearEffectFieldNames[i], result.expect[i], exit.dirty[i], result.expect[i] ^ exit.dirty[i]);
    if (result.mismatch & (1U << guest_d3d::kClearDirtyWords))
        words += std::format(" {}=predicted {:02X} actual {:02X} xor {:02X}", guest_d3d::kClearEffectFieldNames[guest_d3d::kClearDirtyWords], result.expectByte, exit.allocByte, result.expectByte ^ exit.allocByte);
    const auto& p     = *probe.predicted;
    const auto& entry = probe.entryWatch;
    REXGPU_WARN("Native clear effect mismatch:{} entry dirty G+10 {:016X} G+18 {:016X} G+20 {:016X} G+2ABD {:02X} exit dirty {:016X} {:016X} {:016X} G+2ABD {:02X} calls={} stencil={} fast={} untiled={} tiled_block={} tiled_loop={} tiled_none={} allocates={} entry r1={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r8={:08X} f1={:016X}",
                words,
                entry.dirty[0],
                entry.dirty[1],
                entry.dirty[2],
                entry.allocByte,
                exit.dirty[0],
                exit.dirty[1],
                exit.dirty[2],
                exit.allocByte,
                p.calls,
                p.stencilCalls,
                p.fastCalls,
                p.untiledCalls,
                p.tiledBlockCalls,
                p.tiledLoopCalls,
                p.tiledNoneCalls,
                p.allocates,
                e.sp,
                e.g,
                e.flags,
                e.rect,
                e.color,
                e.stencil,
                std::bit_cast<std::uint64_t>(e.depth));
}
#endif

std::atomic<std::uint32_t> frontbufferResolvesLogged{ 0 };
constexpr std::uint32_t    kFrontbufferResolveLogLimit = 4;

// The destination base is dword 1 of the fetch constant at r6+0x1C, as
// ObserveRendererResolve reads it; the ledger compares its physical form with
// the next VdSwap's frontbuffer.
void observeResolve(CompletionLedger& ledger, const PPCContext& ctx)
{
    const auto                   returnAddress = static_cast<std::uint32_t>(ctx.lr);
    const std::uint32_t          call          = returnAddress >= 4 ? returnAddress - 4 : 0;
    std::optional<std::uint32_t> destination;
    std::array<std::uint32_t, 3> fetch{};
    bool                         fetchRead = ctx.r6.u32 <= 0xFFFFFFFFU - 0x28;
    for (std::uint32_t i = 0; fetchRead && i < fetch.size(); ++i)
        fetchRead = readWord(ctx.r6.u32 + 0x1C + 4 * i, fetch[i]);
    if (fetchRead)
        destination = physicalBase(fetch[1] & 0xFFFFF000U);
    resolveDraft = {};
    auto& draft  = resolveDraft;
    draft.g      = ctx.r3.u32;
    draft.flags  = ctx.r4.u32;
    draft.valid  = destination.has_value() && guest_d3d::ReadGuestResolveEntry(readWord,
                                                                               { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 },
                                                                               draft.target,
                                                                               draft.copy,
                                                                               &draft.detail);
    if (guest_d3d::ResolveDeriveCheckRequested() || guest_d3d::ResolveEffectCheckRequested() || guest_d3d::ResolveConsumeRequested()) [[unlikely]]
        beginResolveProbe(ctx, draft.valid);
    ledger.Resolve(call, destination);
    // Without an assembler no resolve form has a native route, so the copy
    // hook could only report the same verdict later.
    if (!currentAssembler())
        ledger.FinishResolve(0, false, false, "resolve_no_native_assembler");
    else if (!draft.valid)
        ledger.FinishResolve(0, false, false, draft.detail.empty() ? std::string("resolve_state_unreadable") : "resolve_state_unreadable: " + draft.detail);
    else
        draft.open = true;
}

std::atomic<std::uint32_t> refusedClearsLogged{ 0 };
constexpr std::uint32_t    kRefusedClearLogLimit = 8;

// The raw words of a clear that did not decode, so a refusal names what the
// guest bound: RT0..RT3 and depth (surface, +0x1C info, +0x18 surface info,
// +0x24 extent) and the tiling byte, count, mask and predicate.
void logRefusedClear(const std::array<std::uint32_t, 8>& r, const std::string& error)
{
    if (refusedClearsLogged.fetch_add(1) >= kRefusedClearLogLimit)
        return;
    const auto value = [](std::uint32_t address)
    {
        std::uint32_t word = 0;
        return readWord(address, word) ? std::format("{:08X}", word) : std::string("unread");
    };
    const std::uint32_t g = r[0];
    std::string         surfaces;
    for (std::uint32_t slot = 0; g <= 0xFFFFFFFFU - 0x3384 && slot < 5; ++slot)
    {
        const std::uint32_t pointer = g + (slot < 4 ? 0x3098 + 4 * slot : 0x30A8);
        std::uint32_t       surface = 0;
        surfaces += std::format(" {}={}", slot < 4 ? std::format("rt{}", slot) : std::string("depth"), value(pointer));
        if (readWord(pointer, surface) && surface && surface <= 0xFFFFFFFFU - 0x28)
            surfaces += std::format("[info={} msaa={} extent={}]", value(surface + 0x1C), value(surface + 0x18), value(surface + 0x24));
    }
    REXGPU_WARN("Native clear refused: {} index={:#x} flags={:08X} rect={},{},{},{}{} tiling={} count={} mask={} predicated={}",
                error,
                r[2],
                r[1],
                static_cast<std::int32_t>(r[3]),
                static_cast<std::int32_t>(r[4]),
                static_cast<std::int32_t>(r[5]),
                static_cast<std::int32_t>(r[6]),
                surfaces,
                value(g + 0x2ABC),
                value(g + 0x31CC),
                value(g + 0x31A4),
                value(g + 0x31AC));
}

std::atomic<std::uint32_t> tiledResolvesLogged{ 0 };
constexpr std::uint32_t    kTiledResolveLogLimit = 16;

// The copy words of a resolve from a tiled off-screen set, which no capture
// records: the set's surfaces, the source slot and the tile its predicate selects.
void logTiledResolve(const guest_d3d::GuestTarget& target, const guest_d3d::GuestResolveCopy& copy, std::uint64_t mark, const std::string& error)
{
    if (tiledResolvesLogged.fetch_add(1) >= kTiledResolveLogLimit)
        return;
    std::string set;
    for (std::uint32_t i = 0; i < target.colorCount; ++i)
        set += std::format("{}{:08X}", i ? "," : "", target.surfaces[i]);
    std::string tile = "none";
    for (std::uint32_t i = 0; i < target.tiling->count && i < target.tiling->rects.size(); ++i)
        if (target.tiling->predicated == 1 && target.tiling->predicateMask == 3U << (2 * i))
            tile = std::to_string(i);
    REXGPU_INFO("Native tiled resolve: set={} slot={} tile={} rect={},{},{},{} point={},{} control={:08X} dest={:08X} info={:08X} fetch2={:08X} depth_clear={:08X} color_clear={:08X} op={} error={}",
                set,
                target.slot,
                tile,
                copy.rectX,
                copy.rectY,
                copy.rectX + copy.rectWidth,
                copy.rectY + copy.rectHeight,
                copy.destX,
                copy.destY,
                copy.copyControl,
                copy.destBase,
                copy.destInfo,
                copy.destFetch[2],
                copy.depthClear,
                copy.colorClear,
                mark,
                error);
}

// 0x8269BE70 waits for the fence in r4 by polling *(B+0), B = *(G+0x2A90);
// the debt report reads the same word.
void observeFenceWait(CompletionLedger& ledger, const PPCContext& ctx)
{
    std::uint32_t block = 0;
    std::uint32_t known = 0;
    if (ctx.r3.u32 <= 0xFFFFFFFFU - 0x2A90 && readWord(ctx.r3.u32 + 0x2A90, block))
        known = physicalAddress(block).value_or(0);
    ledger.RequestFence(ctx.r4.u32, known);
}

// The suspend flag G+0x2ABE bit 0x2. The worker's bracket holds G+0x3A60,
// which resume also takes, so the flag cannot change inside it.
std::optional<bool> suspendFlag(std::uint32_t g)
{
    std::uint8_t flags = 0;
    if (g > 0xFFFFFFFFU - 0x2ABE || !readByte(g + 0x2ABE, flags))
        return std::nullopt;
    return (flags & 2) != 0;
}

// Per-target 0x826A1D90 reads stencil/color from the body frame. Consumed Clear
// never ran that body, so it supplies host-derived words to the shared renderer
// observation.
void settleClear(const std::array<std::uint32_t, 8>& arguments, std::uint32_t stack, double depth, const guest_d3d::GuestClearInputs* supplied) noexcept
{
    auto ledger = currentLedger();
    if (!ledger)
        return;
    try
    {
        const auto            assembler = currentAssembler();
        guest_d3d::GuestClear clear;
        std::string           error;
        std::uint64_t         mark = 0;
        if (!assembler)
            error = "clear_state_unreadable";
        else if (!guest_d3d::ReadGuestClear(readWord, arguments, stack, depth, clear, &error, supplied))
        {
            if (error.empty())
                error = "clear_state_unreadable";
            logRefusedClear(arguments, error);
            // Skip mode releases this clear partial, so a later resolve of the
            // target it hit must not replay content from before it.
            guest_d3d::GuestTarget                target;
            std::optional<guest_d3d::GuestTarget> decoded;
            if (guest_d3d::ReadGuestTarget(readWord, arguments[0], arguments[2], target))
                decoded = target;
            assembler->ClearUnreadable(decoded);
        }
        else
            mark = assembler->Clear(clear, error);
        ledger->Clear(mark, error);
        if (clear.predicate && clear.tiling.Active() && clear.clear.color)
            logicalClear = clear.clear.colorValue;
        static std::atomic<unsigned> logged{ 0 };
        if (logged.fetch_add(1) < 8)
            REXGPU_INFO("Native clear: target={:08X} edram={:03X} extent={}x{} samples={} surface={}x{} tiles={} mask={:X} predicated={} op={} flags={:08X} depth_edram={} rect={},{},{},{} color={},{},{},{} depth={} stencil={} error={}",
                        clear.target.surface,
                        clear.target.native.edramBase,
                        clear.target.native.width,
                        clear.target.native.height,
                        clear.target.native.sampleCount,
                        clear.target.surfaceWidth,
                        clear.target.surfaceHeight,
                        clear.tiling.Active() ? clear.tiling.count : 0,
                        clear.tiling.predicateMask,
                        clear.tiling.predicated,
                        mark,
                        clear.flags,
                        clear.depth ? std::format("{:03X}", clear.depth->edramBase) : std::string("none"),
                        clear.clear.rectangle.left,
                        clear.clear.rectangle.top,
                        clear.clear.rectangle.right,
                        clear.clear.rectangle.bottom,
                        clear.clear.colorValue[0],
                        clear.clear.colorValue[1],
                        clear.clear.colorValue[2],
                        clear.clear.colorValue[3],
                        clear.clear.depthValue,
                        clear.clear.stencilValue,
                        error);
    }
    catch (...)
    {
        ledger->Fail("clear_capture_failed", ProgressKind::Clear, 0x826A1D90);
    }
}

} // namespace

namespace detail
{

void RecordGuestClear(const std::array<std::uint32_t, 8>& arguments, std::uint32_t stack, double depth) noexcept
{
    // Deferred draws before the clear reach the assembler first.
    guest_d3d::DrainNativeDraws();
    if (clearProbes.Size()) [[unlikely]]
    {
        try
        {
            observeClearDerive(arguments, stack, depth);
        }
        catch (...)
        {
            clearProbes.Clear();
        }
    }
    settleClear(arguments, stack, depth, nullptr);
}

// The seven device words ReadGuestResolveCopy reads.
static void copyDeviceWords(guest_d3d::GuestResolveCopy& to, const guest_d3d::GuestResolveCopy& from) noexcept
{
    to.copyControl   = from.copyControl;
    to.destBase      = from.destBase;
    to.destPitch     = from.destPitch;
    to.destInfo      = from.destInfo;
    to.depthClear    = from.depthClear;
    to.colorClear    = from.colorClear;
    to.colorClearLow = from.colorClearLow;
}

// Copy hook 0x8269E8A0 shares ledger/assembler work with consumed Resolve.
// Hooked calls read device words; consumed calls use derived copy words and
// entry clear words, before body clear stores.
static void settleResolveCopy(std::uint32_t g, const guest_d3d::GuestResolveCopy* supplied) noexcept
{
    // The guest writes the copy's destination here whether or not the ledger
    // settles it, so every resolve the assembler does not model is reported,
    // as a write anywhere when its words are unread.
    std::shared_ptr<guest_d3d::IGuestRenderSink> assembler;
    try
    {
        assembler = currentAssembler();
    }
    catch (...)
    {
    }
    const auto unmodeled = [&](std::optional<std::uint32_t> at) noexcept
    {
        if (!assembler)
            return;
        try
        {
            guest_d3d::GuestResolveCopy copy;
            const bool                  read = at && (supplied ? (copy = *supplied, true) : guest_d3d::ReadGuestResolveCopy(readWord, *at, copy));
            assembler->ResolveUnmodeled(read ? std::optional(copy) : std::nullopt);
        }
        catch (...)
        {
            try
            {
                assembler->ResolveUnmodeled(std::nullopt);
            }
            catch (...)
            {
            }
        }
    };
    auto ledger = currentLedger();
    if (!ledger)
    {
        resolveDraft = {};
        unmodeled(g);
        return;
    }
    try
    {
        auto draft = std::exchange(resolveDraft, {});
        // Nothing to settle when the entry observer already did, or when the
        // ledger stopped taking records or abandoned this resolve.
        if (!draft.open || !ledger->AwaitingResolveCopy())
        {
            unmodeled(g);
            return;
        }
        std::string   error;
        std::uint64_t mark    = 0;
        bool          partial = false, frontbuffer = false, copied = false, routed = false;
        if (!assembler)
            error = "resolve_no_native_assembler";
        else if (draft.g != g || !(supplied ? (copyDeviceWords(draft.copy, *supplied), true) : guest_d3d::ReadGuestResolveCopy(readWord, g, draft.copy)))
            error = "resolve_state_unreadable";
        else
        {
            copied = true;
            // MRT slots, depth sources, unpresented forms, and tiled color
            // tiles never become scanout candidates.
            frontbuffer             = assembler->ScanoutCandidate(draft.target, draft.copy);
            const bool    tiled     = !frontbuffer && draft.target.tiling.has_value();
            std::uint32_t predicate = 0, tiling = 0;
            // A tiled off-screen source may carry its slot, fragment select and
            // clear bits; the assembler checks them against the copy words.
            if (tiled && (draft.flags & ~0x377U))
                error = std::format("resolve_flags_unsupported {:#x}", draft.flags);
            else if (!frontbuffer && !tiled && draft.flags != 0)
                error = "resolve_flags_unsupported";
            else if (!frontbuffer && !tiled &&
                     (!readWord(g + 0x31AC, predicate) || !readWord(g + 0x2ABC, tiling) || predicate || ((tiling >> 24) & 0x30)))
                error = "resolve_predicate_unsupported";
            else
            {
                // The tiling, with the predicate as read at entry, travels in the target.
                routed = true;
                mark   = assembler->Resolve(draft.target, draft.copy, frontbuffer, partial, error);
            }
            if (frontbuffer && draft.target.tiling)
                logTiledScanout(*draft.target.tiling, draft.copy);
            if (tiled)
                logTiledResolve(draft.target, draft.copy, mark, error);
        }
        // A resolve refused here still wrote guest memory, perhaps a frontbuffer.
        if (assembler && !routed)
            assembler->ResolveUnmodeled(copied ? std::optional(draft.copy) : std::nullopt);
        ledger->FinishResolve(mark, frontbuffer, partial, error);
        if (frontbufferResolvesLogged.fetch_add(1) < kFrontbufferResolveLogLimit)
            REXGPU_INFO("Native resolve: target={:08X} edram={:03X} extent={}x{} samples={} op={} copy={:08X},{:08X},{:08X},{:08X} physical={:08X} frontbuffer={} error={}",
                        draft.target.surface,
                        draft.target.native.edramBase,
                        draft.target.native.width,
                        draft.target.native.height,
                        draft.target.native.sampleCount,
                        mark,
                        draft.copy.copyControl,
                        draft.copy.destBase,
                        draft.copy.destPitch,
                        draft.copy.destInfo,
                        draft.copy.destBase,
                        frontbuffer,
                        error);
    }
    catch (...)
    {
        unmodeled(std::nullopt);
        ledger->Fail("resolve_capture_failed", ProgressKind::Resolve, 0x8269E8A0);
    }
}

void RecordGuestResolveCopy(std::uint32_t g, std::uint32_t frame) noexcept
{
    guest_d3d::DrainNativeDraws();
    if (resolveProbes.Size()) [[unlikely]]
    {
        try
        {
#if REREVVED_DEVELOPER
            compareResolveDerive(g, frame);
#else
            unsigned dropped = 0;
            resolveProbes.Find(frame, dropped);
#endif
        }
        catch (...)
        {
            resolveProbes.Clear();
        }
    }
    settleResolveCopy(g, nullptr);
}

// Resolve return 0x8269F360 sees body r1 after clear stores
// 0x8269DF00/0x8269E448: changed clear words were previous-call values at copy
// hook. All paths reach this return; higher stacked probes are stale.
void RecordGuestResolveReturn(std::uint32_t frame) noexcept
{
    // Every Resolve path reaches 0x8269F360(g167:14640, 15116, 15126); anything
    // stacked above its frame left another way.
    if (!resolveProbes.Size())
        return;
    try
    {
        unsigned dropped = 0;
#if REREVVED_DEVELOPER
        guest_d3d::ResolveDropCounts drops;
        auto                         armed = resolveProbes.Pop(frame, dropped, &drops);
        guest_d3d::NoteResolveUnpaired(drops.derive);
        guest_d3d::NoteResolveEffectUnpaired(drops.effects);
        if (armed && armed->effects)
            compareResolveEffects(*armed);
        if (!armed || !armed->admitted || !armed->deriveCopy || !armed->hookRead)
            return;
        std::array<std::uint32_t, 3> now{};
        if (armed->g > 0xFFFFFFFFU - 0x2A34 || !readWord(armed->g + 0x2A2C, now[0]) || !readWord(armed->g + 0x2A30, now[1]) || !readWord(armed->g + 0x2A34, now[2]))
            return;
        const bool stale = now != armed->hookClear;
        guest_d3d::NoteResolveClearWords(stale);
        if (stale && resolveClearStaleLogged.fetch_add(1) < kResolveClearStaleLogLimit)
            REXGPU_INFO("Native resolve clear words stale at the copy hook: hook depth={:08X} color={:08X} low={:08X} return depth={:08X} color={:08X} low={:08X}",
                        armed->hookClear[0],
                        armed->hookClear[1],
                        armed->hookClear[2],
                        now[0],
                        now[1],
                        now[2]);
#else
        resolveProbes.Pop(frame, dropped);
#endif
    }
    catch (...)
    {
        resolveProbes.Clear();
    }
}

bool ConsumeGuestResolve(const PPCContext& ctx, bool diagnostics) noexcept
{
    if (!guest_d3d::ResolveConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const guest_d3d::RingStubLease lease;
    if (!lease) [[unlikely]]
        return false;
    const std::array<std::uint32_t, 8>   r{ ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    const auto                           frame = ctx.r1.u32 - guest_d3d::kResolveFrameSize;
    guest_d3d::ResolvePlan               plan;
    std::vector<guest_d3d::ResolveWrite> writes;
    try
    {
        guest_d3d::ResolveConsumeState state;
        state.stubsReady = guest_d3d::RingStubsReady(r[0]);
        state.tiled      = guest_d3d::ResolveTiledConsumeRequested();
        state.pcr        = ctx.r13.u32;
        state.sp         = ctx.r1.u32;
        {
            // The entry observer stacked this call's probe, so any other probe
            // is a Resolve of this thread that has not returned.
            unsigned   dropped = 0;
            const bool own     = resolveProbes.Find(frame, dropped) != nullptr;
            state.nested       = own ? resolveProbes.Size() > 1 : resolveProbes.Size() > 0;
        }
        const auto ledger = currentLedger();
        state.ledgerReady = ledger && currentAssembler();
        state.entryState  = ledger && resolveDraft.valid && resolveDraft.open && resolveDraft.g == r[0] && ledger->AwaitingResolveCopy();
        state.diagnostics = diagnostics || guest_d3d::ResolveDeriveCheckRequested() || guest_d3d::ResolveEffectCheckRequested();
        const guest_d3d::ResolveClearArgs clear{ ctx.r1.u32, ctx.f1.f64 };
        guest_d3d::ResolveShape           shape;
        const auto                        verdict = guest_d3d::ResolveConsumeVerdict(state, readWord, r, &plan, &clear, &shape);
        guest_d3d::NoteResolveShape(shape);
        if (verdict)
        {
            guest_d3d::NoteResolveFallback(*verdict);
            return false;
        }
        writes = guest_d3d::ResolveExitWrites(r[0], r[3], plan.watch, plan.exit);
        // A store that cannot land would leave a half-replayed call, so check them all first.
        auto* memory = guestMemory();
        for (const auto& write : writes)
            if (!writable(memory, write.address, write.byte ? 1 : 4))
            {
                guest_d3d::NoteResolveFallback(guest_d3d::ResolveFallback::Unreadable);
                return false;
            }
    }
    catch (...)
    {
        return false;
    }
    // From here on the call is replayed and Original is skipped; each step
    // contains its own failures.
    guest_d3d::DrainNativeDraws();
    const auto copy = guest_d3d::ResolvePlanCopy(plan);
    settleResolveCopy(r[0], &copy);
    RecordGuestResolveReturn(frame);
    for (const auto& write : writes)
        writeGuest(write);
    guest_d3d::NoteResolveConsumed(plan.tiled, plan.predicated);
    return true;
}

bool ConsumeGuestClear(const PPCContext& ctx, bool diagnostics) noexcept
{
    if (!guest_d3d::ClearConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const guest_d3d::RingStubLease lease;
    if (!lease) [[unlikely]]
        return false;
    const guest_d3d::ClearEntry        entry{ ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, ctx.r8.u32, ctx.r13.u32 };
    const auto                         frame = ctx.r1.u32 - guest_d3d::kClearFrameSize;
    guest_d3d::ClearPlan               plan;
    std::vector<guest_d3d::ClearWrite> writes;
    try
    {
        guest_d3d::ClearConsumeState state;
        state.stubsReady = guest_d3d::RingStubsReady(entry.g);
        state.tiled      = guest_d3d::ClearTiledConsumeRequested();
        {
            // The entry observer stacked this call's placeholder (or its probe), so
            // any other probe is a Clear of this thread that has not returned.
            unsigned   dropped = 0;
            const bool own     = clearProbes.Find(frame, dropped) != nullptr;
            state.entryState   = own;
            state.nested       = own ? clearProbes.Size() > 1 : clearProbes.Size() > 0;
        }
        const auto ledger = currentLedger();
        state.ledgerReady = ledger && currentAssembler();
        // The probes and the harvest, capture and route trace observers read what only the body and its
        // midasm hooks produce.
        state.diagnostics  = diagnostics || guest_d3d::ClearDeriveCheckRequested() || guest_d3d::ClearEffectCheckRequested();
        const auto verdict = guest_d3d::ClearConsumeVerdict(state, readWord, entry, &plan);
        if (verdict)
        {
            guest_d3d::NoteClearFallback(*verdict);
            return false;
        }
        writes = guest_d3d::ClearExitWrites(entry.g, plan.watch, plan.exit);
        // A store that cannot land would leave a half-replayed call, so check them all first.
        auto* memory = guestMemory();
        for (const auto& write : writes)
            if (!writable(memory, write.address, write.byte ? 1 : 4))
            {
                guest_d3d::NoteClearFallback(guest_d3d::ClearFallback::Unreadable);
                return false;
            }
    }
    catch (...)
    {
        return false;
    }
    // From here on the call is replayed and Original is skipped; each step
    // contains its own failures. The return hook of Original never runs, so the
    // placeholder is closed here.
    {
        unsigned dropped = 0;
        clearProbes.Pop(frame, dropped);
    }
    for (std::size_t i = 0; i < plan.calls.count; ++i)
    {
        const auto inputs = guest_d3d::ClearPlanInputs(plan, i);
        // Deferred draws before each clear reach the assembler first, as at the hook.
        guest_d3d::DrainNativeDraws();
        settleClear(guest_d3d::ClearPlanArguments(plan, i), frame, std::bit_cast<double>(plan.calls.calls[i].depthBits), &inputs);
    }
    for (const auto& write : writes)
        writeGuest({ write.address, write.value, write.byte });
    guest_d3d::NoteClearConsumed(plan.exit.shape);
    return true;
}

// Clear body 0x826A22F8 exits at 0x826A2960 with body r1; all paths branch
// here(g158:13027, 13159, 13163). Probes above the matching call left another
// way and are stale.
void RecordGuestClearReturn(std::uint32_t frame) noexcept
{
    if (!clearProbes.Size())
        return;
    try
    {
        unsigned dropped = 0;
#if REREVVED_DEVELOPER
        guest_d3d::ClearDropCounts drops;
        auto                       popped = clearProbes.Pop(frame, dropped, &drops);
        guest_d3d::NoteClearUnpaired(drops.derive);
        guest_d3d::NoteClearEffectUnpaired(drops.effects);
        if (popped && popped->live)
        {
            if (popped->deriveCalls)
                compareClearDerive(*popped);
            if (popped->effects)
                compareClearEffects(*popped);
        }
#else
        clearProbes.Pop(frame, dropped);
#endif
    }
    catch (...)
    {
        clearProbes.Clear();
    }
}

// 0x826A4150 with no flip address: SCRATCH_REG1 = 1 written back to S2+4 and
// a wait for it at 0x826A419C, then a wait for S2+4 == 0 at 0x826A42BC
// after the flip callback. Both sites read S2 from G+0x2A94 (r31 = G).
void RecordPresentFlip(std::uint32_t g, bool request) noexcept
{
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        const std::uint32_t site    = request ? 0x826A419C : 0x826A42BC;
        std::uint32_t       scratch = 0;
        const auto          block   = g <= 0xFFFFFFFFU - 0x2A94 && readWord(g + 0x2A94, scratch) ? physicalAddress(scratch) : std::nullopt;
        if (!block)
        {
            ledger->Fail("present_scratch_unreadable", ProgressKind::GpuWait, site);
            return;
        }
        if (refuseTiled(*ledger, g, request ? ProgressKind::ScratchWrite : ProgressKind::GpuWait, site))
            return;
        const auto flip = *block + kScratchFlipOffset;
        if (request)
        {
            ledger->ScratchWrite(flip, 1, site);
            ledger->GpuWait(flip, 1, site);
        }
        else
            ledger->GpuWait(flip, 0, site);
    }
    catch (...)
    {
    }
}

void RecordUnmodeled(std::uint32_t site) noexcept
{
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        // These sites hold no G in a register; an unreadable device still fails as unmodeled.
        const auto device = globalDevice();
        if (device && tiling(*device).value_or(false))
            ledger->TiledProgress(ProgressKind::Unmodeled, site);
        else
            ledger->Unmodeled(site);
    }
    catch (...)
    {
    }
}

// 0x826A9BA0(G, va) at 0x826A9BDC: EVENT_WRITE_SHD with initiator 0x80000003
// stores the GPU counter at phys(va), a word in B+0x40..0x5F that the swap
// path 0x826A4638 zeroed first.
void RecordGpuTimestamp(std::uint32_t address, std::uint32_t g) noexcept
{
    constexpr std::uint32_t kEmitter = 0x826A9BDC;
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        std::uint32_t block = 0;
        const auto    physicalBlock =
            g <= 0xFFFFFFFFU - 0x2A90 && readWord(g + 0x2A90, block) ? physicalAddress(block) : std::nullopt;
        if (!physicalBlock)
        {
            ledger->Fail("timestamp_block_unreadable", ProgressKind::ScratchWrite, kEmitter);
            return;
        }
        if (refuseTiled(*ledger, g, ProgressKind::ScratchWrite, kEmitter))
            return;
        ledger->GpuTimestamp(guestPhysical(address), *physicalBlock);
    }
    catch (...)
    {
    }
}

void RecordTiledSegment(std::uint32_t physical, std::uint32_t dwords) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->TiledSegment(physical, dwords & 0x00FFFFFFU);
    }
    catch (...)
    {
    }
}

void BeginTiledList() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->BeginTiledList();
    }
    catch (...)
    {
    }
}

void RecordRingConsumeFault(std::string_view step, std::uint32_t site) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->Fail(std::format("ring_consume_fault: {}", step), ProgressKind::Unmodeled, site, false);
    }
    catch (...)
    {
    }
}

void RecordDeferredKick(std::uint32_t physical, std::uint32_t dwords) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->Defer(physical, dwords & 0x00FFFFFFU);
    }
    catch (...)
    {
    }
}

void RecordDirectSubmit(std::uint32_t physical, std::uint32_t dwords) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->DirectSubmit(physical, dwords & 0x00FFFFFFU);
    }
    catch (...)
    {
    }
}

void RecordSilentSubmit() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->SilentSubmit();
    }
    catch (...)
    {
    }
}

void RecordReplaySubmit(std::uint32_t physical, std::uint32_t dwords, std::uint32_t site) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->ReplaySubmit(physical, dwords & 0x00FFFFFFU, site);
    }
    catch (...)
    {
    }
}

void MarkReplayWorker() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->MarkReplayThread();
    }
    catch (...)
    {
    }
}

// 0x8268CF60 stores G+0x2ABE |= 2 at 0x8268CFD4 and moves the device owner
// G+0x2A88 to the worker. Each distinct caller is logged once; the saved lr
// is its return address.
void RecordDeviceSuspend(std::uint32_t stack) noexcept
{
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        ledger->Suspend();
        static std::mutex              callersMutex;
        static std::set<std::uint32_t> callers;
        std::uint32_t                  caller = 0;
        if (stack > 0xFFFFFFFFU - 104 || !readWord(stack + 104, caller))
            caller = 0;
        bool first = false;
        {
            std::lock_guard lock(callersMutex);
            first = callers.insert(caller).second;
        }
        if (first)
            REXGPU_INFO("device_suspend caller={:#010x} frame={}", caller, ledger->Snapshot().swaps);
    }
    catch (...)
    {
    }
}

// 0x8268CFF8 clears G+0x2ABE bit 0x2 at 0x8268D05C and takes the device back.
void RecordDeviceResume() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->Resume();
    }
    catch (...)
    {
    }
}

void RecordDeviceAcquire() noexcept
{
    guest_d3d::DrainNativeDraws();
    try
    {
        if (auto ledger = currentLedger())
            ledger->AcquireDevice();
        if (auto assembler = currentAssembler())
            assembler->DeviceAcquired();
    }
    catch (...)
    {
    }
}

void RecordDeviceRelease() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->ReleaseDevice();
    }
    catch (...)
    {
    }
}

void BeginDeferredSwap(std::uint32_t g) noexcept
{
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        const auto suspended = suspendFlag(g);
        if (!suspended)
            ledger->Fail("deferred_swap_flag_unreadable", ProgressKind::Swap, 0x826ABF7C);
        else if (!*suspended)
            ledger->Fail("deferred_swap_flag_clear", ProgressKind::Swap, 0x826ABF7C);
        else
            ledger->BeginDeviceOwnerSwap();
    }
    catch (...)
    {
    }
}

// The worker also reaches 0x826ABF94 from every idle timeout that found the
// flag clear; no bracket is open then.
void EndDeferredSwap(std::uint32_t g) noexcept
{
    try
    {
        auto ledger = currentLedger();
        if (!ledger)
            return;
        const auto suspended = suspendFlag(g);
        if (!suspended)
            ledger->Fail("deferred_swap_flag_unreadable", ProgressKind::Swap, 0x826ABF94);
        else if (*suspended)
            ledger->EndDeviceOwnerSwap();
    }
    catch (...)
    {
    }
}

void RecordBarrier(std::uint32_t site) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->Barrier(site);
    }
    catch (...)
    {
    }
}

// Swap 0x826A4638 calls 0x826A4458 after VdSwap: lock/unlock AVIVO_D1SCL_UPDATE
// 0x1973, WAIT_REG_MEM until pending bit 0=0, then D1GRPH_FLIP_CONTROL 0x1852.
// GPU-only wait, no guest poll/write: barrier.
void RecordDisplayWait(std::uint32_t site) noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->Barrier(site);
    }
    catch (...)
    {
    }
}

void CloseRingEpochWrites() noexcept
{
    try
    {
        if (auto ledger = currentLedger())
            ledger->CloseEpochWrites();
    }
    catch (...)
    {
    }
}

} // namespace detail

void PublishGuestProgress(std::shared_ptr<CompletionLedger> ledger)
{
    std::lock_guard lock(publicationMutex);
    published = std::move(ledger);
    guestProgressActive.store(published != nullptr, std::memory_order_release);
}

void PublishGuestRenderAssembler(std::weak_ptr<guest_d3d::IGuestRenderSink> assembler)
{
    std::lock_guard lock(publicationMutex);
    publishedAssembler = std::move(assembler);
}

void WithdrawGuestProgress()
{
    std::lock_guard lock(publicationMutex);
    guestProgressActive.store(false, std::memory_order_release);
    published.reset();
}

std::uint64_t GuestProgressReadCount() noexcept
{
    return guestReads;
}

void ObserveGuestProgressEntry(std::uint32_t entry, const PPCContext& ctx) noexcept
{
    try
    {
        // Every Clear reaches this entry, so the disabled probe costs only the
        // cvar test, ahead of the ledger copy. The probe needs no ledger: its
        // hooks only read guest memory.
        if (entry == 0x826A2970)
        {
#if REREVVED_DEVELOPER
            if (guest_d3d::ClearDeriveCheckRequested() || guest_d3d::ClearEffectCheckRequested()) [[unlikely]]
                beginClearDeriveProbe(ctx);
            else
#endif
                if (guest_d3d::ClearConsumeRequested()) [[unlikely]]
                stackClearPlaceholder(ctx);
            return;
        }
        auto ledger = currentLedger();
        if (!ledger)
            return;
        switch (entry)
        {
            case 0x8269BCA8:
                observeFence(*ledger, ctx);
                break;
            case 0x8269BF40:
                observeCallback(*ledger, ctx);
                break;
            case 0x8269E520:
                observeResolve(*ledger, ctx);
                break;
            case 0x8269BE70:
                observeFenceWait(*ledger, ctx);
                break;
            default:
                break;
        }
    }
    catch (...)
    {
        // Bookkeeping never unwinds into guest code.
    }
}

} // namespace rerevved::gpu::completion
