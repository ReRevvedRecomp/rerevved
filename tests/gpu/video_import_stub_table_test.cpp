#include "allocation_harness.hpp"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/video_import_stub_table.h"
#include "manual_cases.hpp"

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/video_delegate.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, native_video_hsio_stub);
REXCVAR_DECLARE(bool, native_video_hsio_exit_check);
REXCVAR_DECLARE(bool, native_video_edram_worker_stub);
REXCVAR_DECLARE(bool, native_video_edram_worker_exit_check);

// The hook adapters the generated code calls (video_import_stub_hooks.cpp).
bool ReplaceNativeVideoHsioCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr);
void ObserveNativeVideoHsioReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13);
void ObserveNativeVideoHsioEffect(PPCRegister& r1, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, PPCCRRegister& cr0);
void ObserveNativeVideoHsioPostChild(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, PPCCRRegister& cr0, uint64_t lr);
bool ReplaceNativeVideoEdramWorkerCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr);
void ObserveNativeVideoEdramWorkerReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13);
void ObserveNativeVideoEdramWorkerPreRetrain(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13);
void ObserveNativeVideoEdramWorkerPostRetrain(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using rex::X_STATUS;
using R  = VideoImportStubRefusal;
using M  = VideoImportMismatch;
using Id = VideoImportId;

constexpr Id kH = Id::Hsio, kW = Id::EdramWorker;

// The independent literals of the two rows. Nothing here is read from the production table.
struct Info
{
    Id            id;
    const char*   name;
    std::uint32_t callPC, truePC, importAddress, frameBytes, returnPC, effectPC, postPC;
    std::uint64_t fullR3, entryLr;
    const char*   prefix;
};

constexpr Info kInfo[2]{
    { kH, "hsio", 0x826A65A4, 0x826A65A8, 0x82E92524, 128, 0x826A65A8, 0x826A65C4, 0x826A65CC, 1, 0x826A6594, "video_hsio_" },
    { kW, "worker", 0x826B053C, 0x826B0540, 0x82E92634, 2192, 0x826B0540, 0x826B0564, 0x826B0568, 0, 0x826B0530, "video_edram_worker_" },
};

const Info& infoOf(Id id)
{
    return kInfo[static_cast<std::size_t>(id)];
}

constexpr std::uint32_t kCache     = 0x82F9B0A4;
constexpr std::uint64_t kHsioR30   = 0xFFFFFFFF82FA0000ULL;
constexpr std::uint64_t kThread    = 0x7001000000000100ULL;
constexpr std::uint64_t kStale     = 0xBADBADBA00000042ULL;
constexpr std::uint64_t kWorkerR30 = 0xC0DE0000DEADBEEFULL;
constexpr std::uint64_t kWorkerR29 = 0xAAAA000000000001ULL;

// One owner invocation's addresses: its stack pointer after the prologue and the device G.
struct Layout
{
    std::uint32_t sp, g;
};

constexpr Layout L0{ 0x80000, 0x20000 };
constexpr Layout L1{ 0x90000, 0x20000 };
constexpr Layout L2{ 0x70000, 0x20000 }; // below L0 on the stack

struct Region
{
    std::uint32_t lo, size;
};

// A guest address space with checked reads, the environment probe and the log.
class Guest
{
public:
    std::mutex                                           mutex;
    std::map<std::uint32_t, std::uint8_t>                bytes;
    std::vector<Region>                                  regions;
    std::set<std::uint32_t>                              unreadable;
    std::atomic<unsigned>                                reads{ 0 }, environments{ 0 };
    std::vector<std::string>                             logs;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stores; // the owner's own stores
    bool                                                 throwOnLog = false;

    // The probe the fake environment returns.
    bool                              runtime = true, graphics = false, delegate = true, ring = true, dispatcher = true, ownersBound = true;
    std::uint64_t                     runtimeId = 0x1001, delegateId = 0x2002, ringId = 0x3003, targetBias = 0;
    std::shared_ptr<int>              delegateObject = std::make_shared<int>(1), ringObject = std::make_shared<int>(2);
    std::map<std::uint32_t, PPCFunc*> targets{ { 0x82E92524, &__imp__VdIsHSIOTrainingSucceeded }, { 0x82E92634, &__imp__VdRetrainEDRAMWorker } };
    std::vector<std::uint32_t>        probedAddresses;

    std::function<void(unsigned)> onEnvironment;
    std::function<void()>         onCommit;
    VideoImportStubOperations     ops;
    bool                          installed = false;

    Guest()                        = default;
    Guest(const Guest&)            = delete;
    Guest& operator=(const Guest&) = delete;

    ~Guest()
    {
        if (installed)
            SetVideoImportOperationsForTesting(nullptr);
    }

    bool readable(std::uint32_t address) const
    {
        for (const auto& region : regions)
            if (address >= region.lo && std::uint64_t{ address } + 4 <= std::uint64_t{ region.lo } + region.size)
                return true;
        return false;
    }

    std::uint8_t rawGet8(std::uint32_t address) const
    {
        const auto it = bytes.find(address);
        return it == bytes.end() ? 0 : it->second;
    }

    std::uint32_t rawGet32(std::uint32_t address) const
    {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4; ++i)
            value = (value << 8) | rawGet8(address + i);
        return value;
    }

    void rawPut32(std::uint32_t address, std::uint32_t value)
    {
        for (std::uint32_t i = 0; i < 4; ++i)
            bytes[address + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }

    std::uint32_t get32(std::uint32_t address)
    {
        std::lock_guard lock(mutex);
        return rawGet32(address);
    }

    void put32(std::uint32_t address, std::uint32_t value)
    {
        std::lock_guard lock(mutex);
        rawPut32(address, value);
    }

    // A store of the owner's code, recorded in the store trace.
    void store32(std::uint32_t address, std::uint32_t value)
    {
        std::lock_guard lock(mutex);
        rawPut32(address, value);
        stores.emplace_back(address, value);
    }

    std::map<std::uint32_t, std::uint8_t> snapshot()
    {
        std::lock_guard lock(mutex);
        return bytes;
    }

    void add(Region region)
    {
        std::lock_guard lock(mutex);
        regions.push_back(region);
    }

    template <typename Change>
    void mutate(const Change& change)
    {
        std::lock_guard lock(mutex);
        change(*this);
    }

    VideoImportStubMemory memory()
    {
        VideoImportStubMemory memory;
        memory.read32 = [this](std::uint32_t address, std::uint32_t& value)
        {
            reads.fetch_add(1);
            std::lock_guard lock(mutex);
            if (!readable(address) || unreadable.count(address))
                return false;
            value = rawGet32(address);
            return true;
        };
        return memory;
    }

    VideoImportEnvironment probe(const VideoImportStubRow& row)
    {
        const auto index = environments.fetch_add(1) + 1;
        if (onEnvironment)
            onEnvironment(static_cast<unsigned>(index));
        std::lock_guard lock(mutex);
        probedAddresses.push_back(row.importAddress);
        VideoImportEnvironment env;
        env.runtime       = runtime;
        env.graphics      = graphics;
        env.delegate      = delegate;
        env.ring          = ring;
        env.dispatcher    = dispatcher;
        const auto it     = targets.find(row.importAddress);
        env.target        = it == targets.end() ? nullptr : it->second;
        env.identity      = { runtimeId, delegate ? delegateId : 0, ring ? ringId : 0, env.target ? reinterpret_cast<std::uintptr_t>(env.target) + targetBias : 0 };
        env.delegateOwner = ownersBound && delegate ? delegateObject : nullptr;
        env.ringOwner     = ownersBound && ring ? ringObject : nullptr;
        return env;
    }

    void install()
    {
        ops = VideoImportStubOperations{ memory(),
                                         [this](const VideoImportStubRow& row)
                                         {
                                             return probe(row);
                                         },
                                         [this](std::string_view message)
                                         {
                                             std::lock_guard lock(mutex);
                                             if (throwOnLog)
                                                 throw std::runtime_error("injected log fault");
                                             logs.emplace_back(message);
                                         },
                                         [this]
                                         {
                                             if (onCommit)
                                                 onCommit();
                                         } };
        installed = true;
        SetVideoImportOperationsForTesting(&ops);
    }

    std::vector<std::string> logLines()
    {
        std::lock_guard lock(mutex);
        return logs;
    }
};

void setup(Guest& guest, const Layout& l)
{
    guest.add({ l.sp - 0x1000, 0x3000 });
    if (!guest.readable(l.g))
        guest.add({ l.g, 0x6000 });
    if (!guest.readable(kCache))
        guest.add({ kCache & ~0xFFFU, 0x1000 });
    for (std::uint32_t offset = 0; offset < 2192; offset += 4)
        guest.put32(l.sp + offset, 0x5A000000 | offset);
    guest.put32(kCache, 0);
}

struct Four
{
    bool hs = false, hc = false, ws = false, wc = false;
};

// One import's pair on, the other import's off.
Four forImport(Id id, bool stub, bool check)
{
    return id == kH ? Four{ stub, check, false, false } : Four{ false, false, stub, check };
}

struct Cvars
{
    Cvars(Four four, bool published, std::uint32_t validated = L0.g)
    {
        REXCVAR_SET(native_video_hsio_stub, four.hs);
        REXCVAR_SET(native_video_hsio_exit_check, four.hc);
        REXCVAR_SET(native_video_edram_worker_stub, four.ws);
        REXCVAR_SET(native_video_edram_worker_exit_check, four.wc);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(published ? validated : 0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(published);
        ResetVideoImportStateForTesting();
    }

    Cvars(Id id, bool stub, bool check, bool published = true, std::uint32_t validated = L0.g)
    : Cvars(forImport(id, stub, check), published, validated)
    {
    }

    ~Cvars()
    {
        REXCVAR_SET(native_video_hsio_stub, false);
        REXCVAR_SET(native_video_hsio_exit_check, false);
        REXCVAR_SET(native_video_edram_worker_stub, false);
        REXCVAR_SET(native_video_edram_worker_exit_check, false);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        SetVideoImportPendingLimitForTesting(kH, kVideoImportMaxPending);
        SetVideoImportPendingLimitForTesting(kW, kVideoImportMaxPending);
        SetVideoImportReserveProbeForTesting(nullptr);
    }
};

// The registers at the hook before the bl: SP after the prologue, G in r31, the row's own
// inputs, and every other register poisoned.
PPCContext makeEntry(Id id, const Layout& l)
{
    PPCContext ctx{};
    std::memset(&ctx, 0x35, sizeof(ctx));
    ctx.r1.u64  = l.sp;
    ctx.r31.u64 = l.g;
    ctx.r13.u64 = kThread;
    ctx.lr      = infoOf(id).entryLr;
    if (id == kH)
    {
        ctx.r3.u64  = kStale;
        ctx.r11.u64 = 0;
        ctx.r30.u64 = kHsioR30;
    }
    else
    {
        ctx.r3.u64  = 0;
        ctx.r29.u64 = kWorkerR29;
        ctx.r30.u64 = kWorkerR30;
    }
    return ctx;
}

// Every register; the struct's tail padding is not guest state and is not copied faithfully.
bool sameContext(const PPCContext& a, const PPCContext& b)
{
    constexpr std::size_t kState = offsetof(PPCContext, v127) + sizeof(PPCVRegister);
    return std::memcmp(&a, &b, kState) == 0;
}

// Equal except LR, which differs by design inside the divergence interval.
bool sameExceptLr(PPCContext a, PPCContext b)
{
    a.lr = b.lr = 0;
    return sameContext(a, b);
}

struct Counts
{
    VideoImportStubCounts counts;

    std::uint64_t refusals() const
    {
        std::uint64_t total = 0;
        for (auto v : counts.refusal)
            total += v;
        return total;
    }

    std::uint64_t refusal(R reason) const
    {
        return counts.refusal[static_cast<std::size_t>(reason)];
    }

    std::uint64_t mismatch(M kind) const
    {
        return counts.mismatch[static_cast<std::size_t>(kind)];
    }

    std::uint64_t mismatches() const
    {
        std::uint64_t total = 0;
        for (auto v : counts.mismatch)
            total += v;
        return total;
    }

    // Every quality counter that must stay zero in a clean run.
    std::uint64_t quality() const
    {
        return counts.exitUnreadable + counts.exitUnpaired + counts.reentry + counts.abandoned + counts.correlationLost + counts.reportingFailed + mismatches();
    }
};

Counts take(Id id)
{
    return { id == kH ? TakeVideoHsioStubCounts() : TakeVideoEdramWorkerStubCounts() };
}

void takeAll()
{
    (void)take(kH);
    (void)take(kW);
}

// Counts and every field that proves a call that ran its import once and was never consumed.
bool isOriginalOnce(const Counts& c)
{
    return c.counts.enter == 1 && c.counts.original == 1 && c.counts.consumed == 0;
}

// The hooks as the generated code calls them.
bool hookReplace(Id id, PPCContext& ctx)
{
    return id == kH ? ReplaceNativeVideoHsioCall(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.lr)
                    : ReplaceNativeVideoEdramWorkerCall(ctx.r1, ctx.r3, ctx.r29, ctx.r30, ctx.r31, ctx.r13, ctx.lr);
}

void hookReturn(Id id, PPCContext& ctx)
{
    if (id == kH)
        ObserveNativeVideoHsioReturn(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13);
    else
        ObserveNativeVideoEdramWorkerReturn(ctx.r1, ctx.r3, ctx.r29, ctx.r30, ctx.r31, ctx.r13);
}

void hookEffect(Id id, PPCContext& ctx)
{
    if (id == kH)
        ObserveNativeVideoHsioEffect(ctx.r1, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.cr0);
    else
        ObserveNativeVideoEdramWorkerPreRetrain(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r29, ctx.r30, ctx.r31, ctx.r13);
}

void hookPost(Id id, PPCContext& ctx)
{
    if (id == kH)
        ObserveNativeVideoHsioPostChild(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.cr0, ctx.lr);
    else
        ObserveNativeVideoEdramWorkerPostRetrain(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r29, ctx.r30, ctx.r31, ctx.r13, ctx.lr);
}

constexpr std::uint64_t addSigned(std::uint64_t base, std::int64_t offset)
{
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + offset);
}

// The registers at each boundary of a call that pairs ok, derived from the entry registers.
PPCContext atReturn(Id id, PPCContext c)
{
    c.r3.u64 = infoOf(id).fullR3;
    return c;
}

PPCContext atEffect(Id id, PPCContext c)
{
    if (id == kH)
    {
        c.r3.u64  = 1;
        c.r11.u64 = 0;
        c.cr0.set_raw(0x2);
        return c;
    }
    c.r11.u64 = 0;
    c.r8.s64  = 2048;
    c.r7.u64  = addSigned(c.r1.u64, 96);
    c.r6.u64  = addSigned(c.r1.u64, 84);
    c.r5.s64  = 4096;
    c.r4.u64  = addSigned(c.r1.u64, 80);
    c.r3.u64  = c.r30.u64;
    return c;
}

PPCContext atPost(Id id, PPCContext c)
{
    if (id == kH)
    {
        c.r3.u64  = 1;
        c.r11.u64 = 0x1234;
        c.cr0.set_raw(0x2);
        c.lr = 0x826A65CC;
        return c;
    }
    c        = atEffect(id, c);
    c.r3.u64 = 0;
    c.lr     = 0x826B0568;
    return c;
}

// The memory the owner has written by the effect boundary.
void owedStores(Guest& guest, Id id, const Layout& l)
{
    if (id == kW)
    {
        guest.put32(l.sp + 80, 0);
        guest.put32(l.sp + 84, 0);
    }
    else
        guest.put32(kCache, 0);
}

// Ends the thread's pairing record of a call at `entry` the way the generated code does after
// the import: the return, the effect or precall observation and the post-child observation.
void finishCall(Guest& guest, Id id, const PPCContext& entry)
{
    owedStores(guest, id, Layout{ entry.r1.u32, entry.r31.u32 });
    auto ctx = atReturn(id, entry);
    hookReturn(id, ctx);
    ctx = atEffect(id, entry);
    hookEffect(id, ctx);
    ctx = atPost(id, entry);
    hookPost(id, ctx);
}

struct Attempt
{
    bool                                  replaced = false;
    unsigned                              reads    = 0; // guest reads made by the hook itself
    PPCContext                            before, after;
    Counts                                counts;
    std::map<std::uint32_t, std::uint8_t> bytes; // guest memory right after the hook
};

// One call of the production adapter, then the observations that close its record.
Attempt attempt(Guest& guest, Id id, PPCContext ctx, const std::atomic<unsigned>* reads = nullptr)
{
    Attempt a;
    a.before   = ctx;
    a.replaced = hookReplace(id, ctx);
    a.after    = ctx;
    a.bytes    = guest.snapshot();
    a.reads    = reads ? reads->load() : 0;
    a.counts   = take(id);
    finishCall(guest, id, ctx);
    takeAll();
    return a;
}

VideoImportStubCall makeCall(const PPCContext& ctx)
{
    VideoImportStubCall call;
    call.r1  = ctx.r1.u64;
    call.r3  = ctx.r3.u64;
    call.r11 = ctx.r11.u64;
    call.r29 = ctx.r29.u64;
    call.r30 = ctx.r30.u64;
    call.r31 = ctx.r31.u64;
    call.r13 = ctx.r13.u64;
    call.lr  = ctx.lr;
    return call;
}

VideoImportStubState goodState(Guest& guest, const VideoImportStubRow& row)
{
    VideoImportStubState state;
    state.lease       = true;
    state.ready       = true;
    state.diagnostics = false;
    state.environment = guest.probe(row);
    return state;
}

const VideoImportStubRow& rowOf(Id id)
{
    const auto* row = FindVideoImportStubRow(infoOf(id).callPC, id);
    if (!row)
        std::abort();
    return *row;
}

// ---------------------------------------------------------------------------
// The owners: the generated sub_826A63E8 (from the cache load) and sub_826B0508
// (from the critical section) over a guest backend.

extern "C" void __imp__VdRetrainEDRAM(PPCContext& ctx, uint8_t* base);

struct Opts
{
    bool                  hooks = true;
    std::function<void()> pauseReturn, pauseEffect, pausePost; // right before the observer
    bool                  skipReturn = false, skipEffect = false, skipPost = false, dupReturn = false;
    std::uint64_t         r13 = kThread, r1High = 0, r31High = 0;
};

struct Owner
{
    PPCContext                                           atEntry{}, atHook{}, atReturn{}, atEffect{}, atChild{}, atRetrain{}, atPost{}, final{};
    std::vector<std::string>                             trace;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stores;
    unsigned                                             importCalls = 0, childCalls = 0, diagCalls = 0, retrainCalls = 0, c810Calls = 0, c7a8Calls = 0;
    bool                                                 replaced = false;
    std::string                                          path;
    std::uint32_t                                        cacheAtEffect = 0;
    std::map<std::uint32_t, std::uint8_t>                memAtReturn, memAtChild;
};

struct HsioModel
{
    std::uint32_t cache        = 0; // the cached training word the owner loads
    std::uint64_t importResult = 1; // the fake import's r3
    std::uint64_t childResult  = 1; // the retained child's r3
    unsigned      so           = 0;
    bool          real         = false;
    std::uint8_t* base         = nullptr;
};

constexpr std::uint32_t kChildStore = 0x82F9B0C0;

Owner runHsioOwner(Guest& g, const Layout& l, const Opts& opts = {}, const HsioModel& model = {})
{
    Owner owner;
    auto  ctx   = makeEntry(kH, l);
    ctx.r13.u64 = opts.r13;
    ctx.r1.u64 |= opts.r1High;
    ctx.r31.u64 |= opts.r31High;
    ctx.xer.so = static_cast<std::uint8_t>(model.so);
    g.put32(kCache, model.cache);
    g.stores.clear();
    // lwz r11,-20316(r30); cmpwi cr6,r11,0.
    ctx.r11.u64 = g.get32(ctx.r30.u32 - 20316);
    ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
    owner.atEntry    = ctx;
    bool diagnostics = false;
    if (ctx.cr6.eq)
    {
        owner.path   = "selected";
        owner.atHook = ctx;
        // loc_826A65A4: bl VdIsHSIOTrainingSucceeded.
        owner.replaced = opts.hooks && hookReplace(kH, ctx);
        if (!owner.replaced)
        {
            ctx.lr = 0x826A65A8;
            ++owner.importCalls;
            owner.trace.push_back("import");
            if (model.real)
                __imp__VdIsHSIOTrainingSucceeded(ctx, model.base);
            else
                ctx.r3.u64 = model.importResult;
        }
        // loc_826A65A8.
        owner.atReturn    = ctx;
        owner.memAtReturn = g.snapshot();
        if (opts.pauseReturn)
            opts.pauseReturn();
        if (opts.hooks && !opts.skipReturn)
            hookReturn(kH, ctx);
        if (opts.hooks && opts.dupReturn)
            hookReturn(kH, ctx);
        ctx.r11.u64 = ctx.r3.u32 == 0 ? 32 : __builtin_clz(ctx.r3.u32);
        ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x1;
        ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
        g.store32(ctx.r30.u32 - 20316, ctx.r11.u32);
        diagnostics = !ctx.cr0.eq;
    }
    else
    {
        owner.path  = "cached";
        diagnostics = true;
    }
    if (diagnostics)
    {
        // loc_826A65B8: the diagnostic call.
        ctx.r11.s64 = -2113339392;
        ctx.r3.s64  = ctx.r11.s64 + 1008;
        ctx.lr      = 0x826A65C4;
        ++owner.diagCalls;
        owner.trace.push_back("diagnostic");
    }
    // loc_826A65C4.
    owner.atEffect      = ctx;
    owner.cacheAtEffect = g.get32(kCache);
    if (opts.pauseEffect)
        opts.pauseEffect();
    if (opts.hooks && !opts.skipEffect)
        hookEffect(kH, ctx);
    ctx.r3.u64       = ctx.r31.u64;
    ctx.lr           = 0x826A65CC;
    owner.atChild    = ctx;
    owner.memAtChild = g.snapshot();
    // bl 0x826B0728: the retained child, with effects of its own.
    ++owner.childCalls;
    owner.trace.push_back("child");
    g.store32(kChildStore, ctx.r31.u32);
    ctx.r3.u64  = model.childResult;
    ctx.r11.u64 = 0x777;
    ctx.r10.u64 = 0x1010;
    ctx.cr0.compare<int32_t>(0, 1, ctx.xer);
    ctx.lr          = 0x826A65CC;
    owner.atRetrain = ctx;
    if (opts.pausePost)
        opts.pausePost();
    owner.atPost = ctx;
    if (opts.hooks && !opts.skipPost)
        hookPost(kH, ctx);
    ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
    if (ctx.cr0.eq)
    {
        ++owner.diagCalls;
        owner.trace.push_back("diagnostic");
    }
    ctx.r3.s64   = 1;
    ctx.r1.s64   = ctx.r1.s64 + 128;
    ctx.lr       = 0x826A4900;
    owner.final  = ctx;
    owner.stores = g.stores;
    return owner;
}

struct WorkerModel
{
    std::uint64_t              r5           = kWorkerR29; // the owner's saved r5, kept in r29
    std::uint64_t              r4           = kWorkerR30; // the owner's saved r4, kept in r30
    std::uint64_t              importResult = 0;
    std::vector<std::uint64_t> retrain{ 0 }; // the retained retrain's r3, call by call
    std::uint64_t              cursor = 0x1000;
    bool                       real   = false;
    std::uint8_t*              base   = nullptr;
};

Owner runWorkerOwner(Guest& g, const Layout& l, const Opts& opts = {}, const WorkerModel& model = {})
{
    Owner owner;
    auto  ctx   = makeEntry(kW, l);
    ctx.r13.u64 = opts.r13;
    ctx.r1.u64 |= opts.r1High;
    ctx.r31.u64 |= opts.r31High;
    ctx.r29.u64 = model.r5;
    ctx.r30.u64 = model.r4;
    g.stores.clear();
    owner.trace.push_back("KeEnterCriticalRegion");
    owner.trace.push_back("RtlEnterCriticalSection");
    // cmpwi cr6,r29,0; beq cr6,loc_826B0540.
    ctx.cr6.compare<int32_t>(ctx.r29.s32, 0, ctx.xer);
    owner.atEntry = ctx;
    if (!ctx.cr6.eq)
    {
        owner.path   = "selected";
        ctx.r3.s64   = 0;
        owner.atHook = ctx;
        // bl VdRetrainEDRAMWorker.
        owner.replaced = opts.hooks && hookReplace(kW, ctx);
        if (!owner.replaced)
        {
            ctx.lr = 0x826B0540;
            ++owner.importCalls;
            owner.trace.push_back("import");
            if (model.real)
                __imp__VdRetrainEDRAMWorker(ctx, model.base);
            else
                ctx.r3.u64 = model.importResult;
        }
    }
    else
        owner.path = "bypass";
    // loc_826B0540.
    owner.atReturn    = ctx;
    owner.memAtReturn = g.snapshot();
    if (opts.pauseReturn)
        opts.pauseReturn();
    if (opts.hooks && !opts.skipReturn)
        hookReturn(kW, ctx);
    if (opts.hooks && opts.dupReturn)
        hookReturn(kW, ctx);
    ctx.r11.s64 = 0;
    ctx.r8.s64  = 2048;
    g.store32(ctx.r1.u32 + 84, ctx.r11.u32);
    ctx.r7.s64 = ctx.r1.s64 + 96;
    g.store32(ctx.r1.u32 + 80, ctx.r11.u32);
    ctx.r6.s64     = ctx.r1.s64 + 84;
    ctx.r5.s64     = 4096;
    ctx.r4.s64     = ctx.r1.s64 + 80;
    ctx.r3.u64     = ctx.r30.u64;
    owner.atEffect = ctx;
    if (opts.pauseEffect)
        opts.pauseEffect();
    if (opts.hooks && !opts.skipEffect)
        hookEffect(kW, ctx);
    // bl VdRetrainEDRAM, retained.
    const auto retrain = [&]
    {
        ctx.lr = 0x826B0568;
        if (owner.retrainCalls == 0)
        {
            owner.atChild    = ctx;
            owner.memAtChild = g.snapshot();
        }
        const auto index = owner.retrainCalls++;
        owner.trace.push_back("retrain");
        if (model.real)
            __imp__VdRetrainEDRAM(ctx, model.base);
        else
            ctx.r3.u64 = index < model.retrain.size() ? model.retrain[index] : 0;
    };
    retrain();
    owner.atRetrain = ctx;
    owner.atPost    = ctx;
    if (opts.pausePost)
        opts.pausePost();
    if (opts.hooks && !opts.skipPost)
        hookPost(kW, ctx);
    ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
    if (!ctx.cr0.eq)
    {
        ctx.cr6.compare<int32_t>(ctx.r29.s32, 0, ctx.xer);
        if (ctx.cr6.eq)
        {
            ctx.r3.u64 = ctx.r31.u64;
            ++owner.c810Calls;
            owner.trace.push_back("C810");
        }
        do
        {
            // loc_826B0580.
            ctx.r4.s64 = 4096;
            ctx.r3.u64 = ctx.r31.u64;
            ++owner.c7a8Calls;
            owner.trace.push_back("C7A8");
            ctx.r3.u64  = model.cursor + owner.c7a8Calls;
            ctx.r11.s64 = ctx.r3.s64 + 4;
            ctx.r8.s64  = 2048;
            g.store32(ctx.r1.u32 + 80, ctx.r11.u32);
            ctx.r7.s64 = ctx.r1.s64 + 96;
            ctx.r6.s64 = ctx.r1.s64 + 84;
            ctx.r5.s64 = 4096;
            ctx.r4.s64 = ctx.r1.s64 + 80;
            ctx.r3.u64 = ctx.r30.u64;
            retrain();
            ctx.r11.u64 = g.get32(ctx.r1.u32 + 80);
            ctx.r29.u64 = ctx.r3.u64;
            ctx.r11.s64 = ctx.r11.s64 + -4;
            ctx.r3.u64  = ctx.r31.u64;
            g.store32(ctx.r1.u32 + 80, ctx.r11.u32);
            g.store32(ctx.r31.u32 + 48, ctx.r11.u32);
            ++owner.c810Calls;
            owner.trace.push_back("C810");
            ctx.cr6.compare<int32_t>(ctx.r29.s32, 0, ctx.xer);
        } while (!ctx.cr6.eq);
    }
    // loc_826B05D4.
    owner.trace.push_back("RtlLeaveCriticalSection");
    owner.trace.push_back("KeLeaveCriticalRegion");
    ctx.r3.s64   = 1;
    ctx.r1.s64   = ctx.r1.s64 + 2192;
    ctx.lr       = 0x826B05E0;
    owner.final  = ctx;
    owner.stores = g.stores;
    return owner;
}

Owner runOwner(Id id, Guest& g, const Layout& l, const Opts& opts = {})
{
    return id == kH ? runHsioOwner(g, l, opts) : runWorkerOwner(g, l, opts);
}

// ---------------------------------------------------------------------------

void testConstants()
{
    // Runtime checks, so that a changed constant still compiles and fails here.
    UNSCOPED_INFO("hsio program counters");
    CHECK((kVideoHsioCallPC == 0x826A65A4 && kVideoHsioReturnPC == 0x826A65A8 && kVideoHsioEffectPC == 0x826A65C4 && kVideoHsioPostPC == 0x826A65CC));
    UNSCOPED_INFO("worker program counters");
    CHECK((kVideoWorkerCallPC == 0x826B053C && kVideoWorkerReturnPC == 0x826B0540 && kVideoWorkerPrePC == 0x826B0564 && kVideoWorkerPostPC == 0x826B0568));
    UNSCOPED_INFO("import and cache addresses");
    CHECK((kVideoHsioImportAddress == 0x82E92524 && kVideoWorkerImportAddress == 0x82E92634 && kVideoHsioCacheAddress == 0x82F9B0A4));
    UNSCOPED_INFO("cache base and frame sizes");
    CHECK((kVideoHsioR30 == 0xFFFFFFFF82FA0000ULL && kVideoHsioFrameBytes == 128 && kVideoWorkerFrameBytes == 2192 && kVideoImportMaxFrameBytes == 2192));
    UNSCOPED_INFO("worker stack offsets");
    CHECK((kVideoWorkerStackOffsetA == 80 && kVideoWorkerStackOffsetB == 84 && kVideoWorkerArgOffset == 96));
    UNSCOPED_INFO("two rows and 64 outstanding calls");
    CHECK((kVideoImportCount == 2 && kVideoImportMaxPending == 64));
    UNSCOPED_INFO("nine refusal reasons and four mismatch kinds");
    CHECK((kVideoImportRefusalCount == 9 && kVideoImportMismatchCount == 4));
    const char* refusals[]   = { "pc", "services", "provider", "diagnostics", "stack", "shape", "unreadable", "reentrant", "unpaired" };
    const char* mismatches[] = { "return", "effect", "continuation", "lr" };
    for (std::size_t i = 0; i < 9; ++i)
    {
        UNSCOPED_INFO("refusal name order");
        CHECK(kVideoImportRefusalNames[i] == refusals[i]);
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        UNSCOPED_INFO("mismatch name order");
        CHECK(kVideoImportMismatchNames[i] == mismatches[i]);
    }
    UNSCOPED_INFO("both HSIO options default to off");
    CHECK((!REXCVAR_GET(native_video_hsio_stub) && !REXCVAR_GET(native_video_hsio_exit_check)));
    UNSCOPED_INFO("both worker options default to off");
    CHECK((!REXCVAR_GET(native_video_edram_worker_stub) && !REXCVAR_GET(native_video_edram_worker_exit_check)));
    UNSCOPED_INFO("the final drain waits at most 10 ms");
    CHECK(kVideoImportShutdownWait == std::chrono::milliseconds(10));
    UNSCOPED_INFO("64 outstanding calls per import at most");
    CHECK(kVideoImportMaxPending == 64);
}

void testTable()
{
    UNSCOPED_INFO("exactly two rows");
    CHECK(kVideoImportStubRows.size() == 2);
    // Each row against independent literals.
    for (const auto& info : kInfo)
    {
        const auto* row = FindVideoImportStubRow(info.callPC, info.id);
        UNSCOPED_INFO(std::string(info.name) + ": the row is found");
        CHECK(row != nullptr);
        if (!row)
            continue;
        UNSCOPED_INFO(std::string(info.name) + ": pc, import, full r3 and continuation");
        CHECK((row->callPC == info.callPC && row->import == info.id && row->fullR3 == info.fullR3 && row->truePC == info.truePC));
        UNSCOPED_INFO(std::string(info.name) + ": import address, frame and readiness");
        CHECK((row->importAddress == info.importAddress && row->frameBytes == info.frameBytes && row->readiness == VideoImportReadiness::RingStubsReady));
        if (info.id == kH)
        {
            UNSCOPED_INFO("hsio: wrapper and predicate");
            CHECK((row->wrapper == &__imp__VdIsHSIOTrainingSucceeded && row->predicate == &VideoHsioPredicate));
            UNSCOPED_INFO("hsio: not the worker's");
            CHECK((row->wrapper != &__imp__VdRetrainEDRAMWorker && row->predicate != &VideoEdramWorkerPredicate));
        }
        else
        {
            UNSCOPED_INFO("worker: wrapper and predicate");
            CHECK((row->wrapper == &__imp__VdRetrainEDRAMWorker && row->predicate == &VideoEdramWorkerPredicate));
            UNSCOPED_INFO("worker: not the HSIO's");
            CHECK((row->wrapper != &__imp__VdIsHSIOTrainingSucceeded && row->predicate != &VideoHsioPredicate));
        }
    }
    // The exact PC and import, never a range, a default or a first row.
    UNSCOPED_INFO("a row's PC with the other import is rejected");
    CHECK((FindVideoImportStubRow(kInfo[0].callPC, kW) == nullptr && FindVideoImportStubRow(kInfo[1].callPC, kH) == nullptr));
    UNSCOPED_INFO("an unknown PC is rejected");
    CHECK((FindVideoImportStubRow(0x826A65A8, kH) == nullptr && FindVideoImportStubRow(0x826A65A0, kH) == nullptr && FindVideoImportStubRow(0, kH) == nullptr && FindVideoImportStubRow(0, kW) == nullptr));
    UNSCOPED_INFO("the neighbours of the worker's PC are rejected");
    CHECK((FindVideoImportStubRow(0x826B0540, kW) == nullptr && FindVideoImportStubRow(0x826B0538, kW) == nullptr));
    UNSCOPED_INFO("the initialize call is not in the table");
    CHECK((FindVideoImportStubRow(0x826A632C, kH) == nullptr && FindVideoImportStubRow(0x826A632C, kW) == nullptr));
    UNSCOPED_INFO("the display mode call is not in the table");
    CHECK((FindVideoImportStubRow(0x826A6260, kH) == nullptr && FindVideoImportStubRow(0x826A6260, kW) == nullptr));
    UNSCOPED_INFO("the owner's entry is not a row");
    CHECK(FindVideoImportStubRow(0x826A63E8, kH) == nullptr);
    UNSCOPED_INFO("unique PCs, imports and import addresses");
    CHECK((kVideoImportStubRows[0].callPC != kVideoImportStubRows[1].callPC && kVideoImportStubRows[0].import != kVideoImportStubRows[1].import && kVideoImportStubRows[0].importAddress != kVideoImportStubRows[1].importAddress));
    // The predicates tell the rows apart.
    auto hsio   = makeCall(makeEntry(kH, L0));
    auto worker = makeCall(makeEntry(kW, L0));
    UNSCOPED_INFO("the HSIO registers satisfy only the HSIO predicate");
    CHECK((VideoHsioPredicate(hsio) && !VideoEdramWorkerPredicate(hsio)));
    UNSCOPED_INFO("the worker registers satisfy only the worker predicate");
    CHECK((VideoEdramWorkerPredicate(worker) && !VideoHsioPredicate(worker)));
    // A row that does not match its lookup is a pc refusal before anything else.
    Guest guest;
    setup(guest, L0);
    for (const auto& info : kInfo)
    {
        const auto&         good  = rowOf(info.id);
        const auto&         other = rowOf(info.id == kH ? kW : kH);
        const auto          call  = makeCall(makeEntry(info.id, L0));
        const auto          state = goodState(guest, good);
        VideoImportStubPlan plan;
        UNSCOPED_INFO(std::string(info.name) + ": the table row derives");
        CHECK(!DeriveVideoImportStub(good, call, state, guest.memory(), plan));
        const auto refuses = [&](VideoImportStubRow row, const char* what)
        {
            VideoImportStubPlan p;
            const auto          before = guest.reads.load();
            const auto          r      = DeriveVideoImportStub(row, call, state, guest.memory(), p);
            UNSCOPED_INFO(std::string(info.name) + ": a row with a different " + what + " is a pc refusal");
            CHECK((r && *r == R::Pc && guest.reads.load() == before && p.row == nullptr));
        };
        auto row = good;
        row.callPC += 4;
        refuses(row, "callPC");
        row        = good;
        row.import = info.id == kH ? kW : kH;
        refuses(row, "import");
        row = good;
        row.fullR3 ^= 1;
        refuses(row, "full r3");
        row = good;
        row.truePC += 4;
        refuses(row, "continuation");
        row           = good;
        row.predicate = other.predicate;
        refuses(row, "predicate");
        row = good;
        row.importAddress += 16;
        refuses(row, "import address");
        row         = good;
        row.wrapper = other.wrapper;
        refuses(row, "wrapper");
        row = good;
        row.frameBytes += 16;
        refuses(row, "frame size");
        row           = good;
        row.readiness = static_cast<VideoImportReadiness>(7);
        refuses(row, "readiness");
        row        = other;
        row.callPC = good.callPC;
        refuses(row, "row of the other import at this PC");
    }
}

void testDeriveAndApply()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        Guest             guest;
        setup(guest, L0);
        const auto&         row    = rowOf(info.id);
        const auto          memory = guest.memory();
        const auto          before = guest.bytes;
        const auto          call   = makeCall(makeEntry(info.id, L0));
        VideoImportStubPlan plan;
        const auto          refusal = DeriveVideoImportStub(row, call, goodState(guest, row), memory, plan);
        UNSCOPED_INFO(name + ": the owner's call derives");
        CHECK(!refusal);
        UNSCOPED_INFO(name + ": the plan freezes the row's constant");
        CHECK((plan.row == &row && plan.fullR3 == info.fullR3 && plan.sp == L0.sp && plan.device == L0.g));
        UNSCOPED_INFO(name + ": the plan keeps the full entry registers");
        CHECK((plan.call.r1 == call.r1 && plan.call.r3 == call.r3 && plan.call.r11 == call.r11 && plan.call.r29 == call.r29 && plan.call.r30 == call.r30 && plan.call.r31 == call.r31 && plan.call.r13 == call.r13 && plan.call.lr == call.lr));
        UNSCOPED_INFO(name + ": the plan binds the identities");
        CHECK((plan.identity.runtime == guest.runtimeId && plan.identity.delegate == guest.delegateId && plan.identity.ring == guest.ringId && plan.identity.target != 0));
        const unsigned words = info.frameBytes / 4;
        UNSCOPED_INFO(name + ": derive reads the whole frame and, for HSIO, the cache word");
        CHECK(guest.reads == words + (info.id == kH ? 1U : 0U));
        UNSCOPED_INFO(name + ": the frame snapshot is valid");
        CHECK(plan.snapshot.frameValid);
        bool same = true;
        for (unsigned i = 0; i < words; ++i)
            same = same && plan.snapshot.frame[i] == guest.get32(L0.sp + 4 * i);
        UNSCOPED_INFO(name + ": the snapshot holds every frame word");
        CHECK(same);
        if (words < plan.snapshot.frame.size())
        {
            UNSCOPED_INFO(name + ": and no word beyond the frame");
            CHECK(plan.snapshot.frame[words] == 0);
        }
        if (info.id == kH)
        {
            UNSCOPED_INFO(name + ": the cache word is snapshotted");
            CHECK((plan.snapshot.cacheValid && plan.snapshot.cache == 0));
        }
        UNSCOPED_INFO(name + ": derive changes no guest byte");
        CHECK(guest.bytes == before);
        VideoImportStubPlan again;
        UNSCOPED_INFO(name + ": derive is repeatable");
        CHECK((!DeriveVideoImportStub(row, call, goodState(guest, row), memory, again) && again.sp == plan.sp && again.identity == plan.identity));

        // The apply seam assigns the whole 64-bit r3 whatever it held, also when the valid
        // incoming r3 is already the constant.
        for (const std::uint64_t value : std::initializer_list<std::uint64_t>{ std::uint64_t{ 0 }, std::uint64_t{ 1 }, 0xFFFFFFFF00000000ULL, 0x00000001FFFFFFFFULL, 0xFFFFFFFF00000001ULL, 0x0000000100000000ULL, ~0ULL, kStale })
        {
            auto r3 = value;
            ApplyVideoImportStub(plan, r3);
            UNSCOPED_INFO(name + ": apply assigns the full r3 constant");
            CHECK(r3 == info.fullR3);
        }
    }
}

void testPredicates()
{
    // HSIO: full r30, the just-loaded r11 zero in 64 bits; the incoming r3 is stale and unconstrained.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kH, true, false);
        for (const std::uint64_t r3 : std::initializer_list<std::uint64_t>{ kStale, std::uint64_t{ 0 }, ~0ULL, 0x0000000100000001ULL })
        {
            auto ctx     = makeEntry(kH, L0);
            ctx.r3.u64   = r3;
            const auto a = attempt(guest, kH, ctx);
            UNSCOPED_INFO("HSIO: any stale incoming r3 is consumed");
            CHECK((a.replaced && a.after.r3.u64 == 1 && a.counts.counts.consumed == 1 && a.counts.refusals() == 0));
        }

        struct Bad
        {
            const char*   name;
            std::uint64_t r30, r11;
        };

        const Bad bad[] = {
            { "r30 low word only", 0x0000000082FA0000ULL, 0 },
            { "r30 high word only", 0xFFFFFFFF00000000ULL, 0 },
            { "r30 off by one", kHsioR30 + 1, 0 },
            { "r30 zero", 0, 0 },
            { "r11 low nonzero", kHsioR30, 1 },
            { "r11 high only", kHsioR30, 0x0000000100000000ULL },
            { "r11 all ones", kHsioR30, ~0ULL },
        };
        for (const auto& b : bad)
        {
            auto ctx     = makeEntry(kH, L0);
            ctx.r30.u64  = b.r30;
            ctx.r11.u64  = b.r11;
            const auto a = attempt(guest, kH, ctx);
            UNSCOPED_INFO(std::string("HSIO: ") + b.name + " is one shape refusal");
            CHECK((!a.replaced && sameContext(a.before, a.after) && a.counts.refusal(R::Shape) == 1 && a.counts.refusals() == 1 && isOriginalOnce(a.counts)));
        }
    }
    // Worker: full r3 zero and a nonzero low word of the saved r5 in r29; everything else is retained or unconstrained.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kW, true, false);
        for (const std::uint64_t r29 : std::initializer_list<std::uint64_t>{ std::uint64_t{ 1 }, std::uint64_t{ 0x7FFFFFFF }, std::uint64_t{ 0xFFFFFFFF }, kWorkerR29, 0xFFFFFFFF00000001ULL })
        {
            auto ctx     = makeEntry(kW, L0);
            ctx.r29.u64  = r29;
            const auto a = attempt(guest, kW, ctx);
            UNSCOPED_INFO("worker: a nonzero low word of r29 is consumed");
            CHECK((a.replaced && a.after.r3.u64 == 0 && a.counts.counts.consumed == 1));
        }
        for (const std::uint64_t r30 : std::initializer_list<std::uint64_t>{ std::uint64_t{ 0 }, ~0ULL, 0x123456789ABCDEF0ULL, kHsioR30 })
        {
            auto ctx     = makeEntry(kW, L0);
            ctx.r30.u64  = r30;
            const auto a = attempt(guest, kW, ctx);
            UNSCOPED_INFO("worker: any retained r30 is consumed");
            CHECK((a.replaced && a.counts.counts.consumed == 1));
        }
        auto ctx   = makeEntry(kW, L0);
        ctx.r4.u64 = 0x1111, ctx.r5.u64 = 0x2222, ctx.r6.u64 = ~0ULL, ctx.r7.u64 = 0, ctx.r8.u64 = 0x8000000000000000ULL, ctx.r10.u64 = 7;
        UNSCOPED_INFO("worker: r4..r10 are unconstrained");
        CHECK(attempt(guest, kW, ctx).replaced);

        struct Bad
        {
            const char*   name;
            std::uint64_t r3, r29;
        };

        const Bad bad[] = {
            { "r3 low nonzero", 1, 1 },
            { "r3 high only", 0x0000000100000000ULL, 1 },
            { "r3 all ones", ~0ULL, 1 },
            { "r29 zero", 0, 0 },
            { "r29 high only", 0, 0x0000000100000000ULL },
            { "r29 high word with a zero low word", 0, 0xFFFFFFFF00000000ULL },
        };
        for (const auto& b : bad)
        {
            auto bctx    = makeEntry(kW, L0);
            bctx.r3.u64  = b.r3;
            bctx.r29.u64 = b.r29;
            const auto a = attempt(guest, kW, bctx);
            UNSCOPED_INFO(std::string("worker: ") + b.name + " is one shape refusal");
            CHECK((!a.replaced && sameContext(a.before, a.after) && a.counts.refusal(R::Shape) == 1 && a.counts.refusals() == 1 && isOriginalOnce(a.counts)));
        }
    }
}

void testRefusalTable()
{
    struct Case
    {
        const char*                                           name;
        R                                                     reason;
        bool                                                  check;
        bool                                                  decisionReadsNothing;
        std::function<void(Guest&, PPCContext&, const Info&)> tweak;
    };

    const auto wrapPoint = [](const Info& info)
    {
        return static_cast<std::uint32_t>(0x100000000ULL - info.frameBytes + 16);
    };
    const Case cases[] = {
        { "device not validated", R::Services, false, false, [](Guest&, PPCContext&, const Info&)
          {
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(L0.g + 4);
          } },
        { "device zero", R::Services, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r31.u64 = 0;
          } },
        { "graphics present", R::Services, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.graphics = true;
          } },
        { "delegate missing", R::Services, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.delegate = false;
          } },
        { "ring service missing", R::Services, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.ring = false;
          } },
        { "runtime missing", R::Services, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.runtime = false;
          } },
        { "ring identity unbound", R::Services, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.ringId = 0;
          } },
        { "dispatcher missing", R::Provider, false, true, [](Guest& g, PPCContext&, const Info&)
          {
              g.dispatcher = false;
          } },
        { "target missing", R::Provider, false, true, [](Guest& g, PPCContext&, const Info& i)
          {
              g.targets.erase(i.importAddress);
          } },
        { "target is the other import's wrapper", R::Provider, false, true, [](Guest& g, PPCContext&, const Info& i)
          {
              g.targets[i.importAddress] = i.id == kH ? &__imp__VdRetrainEDRAMWorker : &__imp__VdIsHSIOTrainingSucceeded;
          } },
        { "target is another function", R::Provider, false, true, [](Guest& g, PPCContext&, const Info& i)
          {
              g.targets[i.importAddress] = &__imp__VdRetrainEDRAM;
          } },
        { "exit check forces the import", R::Diagnostics, true, false, nullptr },
        { "SP zero", R::Stack, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64 = 0;
          } },
        { "SP zero low word with a high word", R::Stack, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64 = 0x0000000100000000ULL;
          } },
        { "SP misaligned", R::Stack, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64 = L0.sp + 8;
          } },
        { "SP frame wraps", R::Stack, false, false, [wrapPoint](Guest&, PPCContext& c, const Info& i)
          {
              c.r1.u64 = wrapPoint(i);
          } },
        { "first frame word unreadable", R::Stack, false, false, [](Guest& g, PPCContext&, const Info&)
          {
              g.unreadable.insert(L0.sp);
          } },
        { "middle frame word unreadable", R::Stack, false, false, [](Guest& g, PPCContext&, const Info& i)
          {
              g.unreadable.insert(L0.sp + (i.frameBytes / 2 & ~3U));
          } },
        { "last frame word unreadable", R::Stack, false, false, [](Guest& g, PPCContext&, const Info& i)
          {
              g.unreadable.insert(L0.sp + i.frameBytes - 4);
          } },
        { "frame not mapped", R::Stack, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64 = 0x40000;
          } },
        { "shape", R::Shape, false, false, [](Guest&, PPCContext& c, const Info& i)
          {
              if (i.id == kH)
                  c.r11.u64 = 4;
              else
                  c.r3.u64 = 4;
          } },
        { "no observation record", R::Unpaired, false, false, nullptr },
        // Several failures at once: the first reason in the order wins.
        { "services before provider and shape", R::Services, false, true, [](Guest& g, PPCContext& c, const Info&)
          {
              g.delegate   = false;
              g.dispatcher = false;
              c.r30.u64    = 0;
              c.r3.u64     = 9;
          } },
        { "provider before diagnostics and stack", R::Provider, true, true, [](Guest& g, PPCContext& c, const Info& i)
          {
              g.targets.erase(i.importAddress);
              c.r1.u64 = L0.sp + 8;
          } },
        { "diagnostics before stack and shape", R::Diagnostics, true, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64  = L0.sp + 8;
              c.r30.u64 = 0;
              c.r3.u64  = 9;
          } },
        { "stack before shape", R::Stack, false, false, [](Guest&, PPCContext& c, const Info&)
          {
              c.r1.u64  = L0.sp + 8;
              c.r30.u64 = 0;
              c.r3.u64  = 9;
          } },
        { "unreadable frame before shape", R::Stack, false, false, [](Guest& g, PPCContext& c, const Info&)
          {
              g.unreadable.insert(L0.sp + 4);
              c.r30.u64 = 0;
              c.r3.u64  = 9;
          } },
    };
    for (const auto& info : kInfo)
        for (const auto& c : cases)
        {
            const std::string name = std::string(info.name) + ": " + c.name;
            Guest             guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, true, c.check);
            auto  ctx = makeEntry(info.id, L0);
            if (c.tweak)
                c.tweak(guest, ctx, info);
            if (std::string_view(c.name) == "no observation record")
                SetVideoImportPendingLimitForTesting(info.id, 0);
            const auto bytes = guest.bytes;
            const auto a     = attempt(guest, info.id, ctx, &guest.reads);
            UNSCOPED_INFO(name + ": not replaced");
            CHECK(!a.replaced);
            UNSCOPED_INFO(name + ": the hook changes no register");
            CHECK(sameContext(a.before, a.after));
            UNSCOPED_INFO(name + ": the slice changes no guest byte");
            CHECK(a.bytes == bytes);
            UNSCOPED_INFO(name + ": exactly that refusal");
            CHECK((a.counts.refusal(c.reason) == 1 && a.counts.refusals() == 1));
            UNSCOPED_INFO(name + ": one entry, one original, no consume");
            CHECK(isOriginalOnce(a.counts));
            if (c.decisionReadsNothing)
            {
                UNSCOPED_INFO(name + ": the decision reads no guest word");
                CHECK(a.reads == 0);
            }
            SetVideoImportPendingLimitForTesting(info.id, kVideoImportMaxPending);
        }

    // The unreadable cache word is the HSIO row's own required entry word.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kH, true, false);
        guest.unreadable.insert(kCache);
        const auto bytes = guest.bytes;
        const auto a     = attempt(guest, kH, makeEntry(kH, L0));
        UNSCOPED_INFO("hsio: an unreadable cache word leaves everything untouched");
        CHECK((!a.replaced && sameContext(a.before, a.after) && guest.bytes == bytes));
        UNSCOPED_INFO("hsio: an unreadable cache word is one unreadable refusal");
        CHECK((a.counts.refusal(R::Unreadable) == 1 && a.counts.refusals() == 1 && isOriginalOnce(a.counts)));
        // The shape wins over the unreadable word, and the unreadable word over the missing record.
        auto bad         = makeEntry(kH, L0);
        bad.r11.u64      = 1;
        const auto shape = attempt(guest, kH, bad);
        UNSCOPED_INFO("hsio: shape before unreadable");
        CHECK((shape.counts.refusal(R::Shape) == 1 && shape.counts.refusals() == 1));
        SetVideoImportPendingLimitForTesting(kH, 0);
        const auto unreadable = attempt(guest, kH, makeEntry(kH, L0));
        UNSCOPED_INFO("hsio: unreadable before unpaired");
        CHECK((unreadable.counts.refusal(R::Unreadable) == 1 && unreadable.counts.refusals() == 1));
    }
    {
        // The worker reads no cache word and the HSIO row reads no word beyond its 128 bytes.
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kW, true, false);
        guest.unreadable.insert(kCache);
        UNSCOPED_INFO("worker: the cache word is not its business");
        CHECK(attempt(guest, kW, makeEntry(kW, L0)).replaced);
        guest.unreadable.clear();
        guest.unreadable.insert(L0.sp + 2192);
        UNSCOPED_INFO("worker: the word after its 2192 bytes is not read");
        CHECK(attempt(guest, kW, makeEntry(kW, L0)).replaced);
        guest.unreadable.clear();
        guest.unreadable.insert(L0.sp + 2188);
        const auto last = attempt(guest, kW, makeEntry(kW, L0));
        UNSCOPED_INFO("worker: the last word of the 2192 bytes is read");
        CHECK((!last.replaced && last.counts.refusal(R::Stack) == 1));
        guest.unreadable.clear();
        guest.unreadable.insert(L0.sp + 128);
        UNSCOPED_INFO("worker: its frame is not the HSIO's 128 bytes");
        CHECK(attempt(guest, kW, makeEntry(kW, L0)).counts.refusal(R::Stack) == 1);
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kH, true, false);
        for (const std::uint32_t offset : { 128U, 132U, 1000U, 2188U })
        {
            guest.unreadable.clear();
            guest.unreadable.insert(L0.sp + offset);
            UNSCOPED_INFO("hsio: a word at or after 128 bytes is not read");
            CHECK(attempt(guest, kH, makeEntry(kH, L0)).replaced);
        }
        guest.unreadable.clear();
        guest.unreadable.insert(L0.sp + 124);
        UNSCOPED_INFO("hsio: the last of its 128 bytes is read");
        CHECK(attempt(guest, kH, makeEntry(kH, L0)).counts.refusal(R::Stack) == 1);
    }
    // A wrong PC is refused before the lease, the environment and any guest read.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, true, false);
        guest.runtime = false;
        static std::atomic<unsigned> leases;
        leases = 0;
        SetRingStubLeaseProbe([]() noexcept
                              {
                                  leases.fetch_add(1);
                              });
        const auto    entry = makeEntry(info.id, L0);
        std::uint64_t r3    = entry.r3.u64;
        const auto    other = infoOf(info.id == kH ? kW : kH);
        UNSCOPED_INFO(std::string(info.name) + ": the other row's PC is not replaced");
        CHECK((!ReplaceVideoImportStubCall(other.callPC, info.id, makeCall(entry), r3) && r3 == entry.r3.u64));
        UNSCOPED_INFO(std::string(info.name) + ": an unknown PC is not replaced");
        CHECK((!ReplaceVideoImportStubCall(info.callPC + 4, info.id, makeCall(entry), r3) && r3 == entry.r3.u64));
        SetRingStubLeaseProbe(nullptr);
        const auto counts = take(info.id);
        UNSCOPED_INFO(std::string(info.name) + ": two pc refusals, two originals");
        CHECK((counts.counts.enter == 2 && counts.refusal(R::Pc) == 2 && counts.refusals() == 2 && counts.counts.original == 2 && counts.counts.consumed == 0));
        UNSCOPED_INFO(std::string(info.name) + ": no lease, environment or read for a pc refusal");
        CHECK((leases.load() == 0 && guest.environments == 0 && guest.reads == 0));
        UNSCOPED_INFO(std::string(info.name) + ": no record, no readiness");
        CHECK((counts.counts.pending == 0 && counts.counts.readyAtEntry == 0));
        takeAll();
    }
}

void testFrameIdentity()
{
    // The raw high words of SP and G are preserved, never normalized.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, true, false);
        auto  ctx           = makeEntry(info.id, L0);
        ctx.r1.u64          = 0x0000000100000000ULL | L0.sp;
        ctx.r31.u64         = 0xABCD000000000000ULL | L0.g;
        const auto a        = attempt(guest, info.id, ctx);
        auto       expected = a.before;
        expected.r3.u64     = info.fullR3;
        UNSCOPED_INFO(std::string(info.name) + ": raw high words of SP and G are accepted and nothing else changes");
        CHECK((a.replaced && sameContext(a.after, expected)));
        UNSCOPED_INFO(std::string(info.name) + ": consumed");
        CHECK((a.counts.counts.consumed == 1 && a.counts.refusals() == 0));
        const auto lines = guest.logLines();
        UNSCOPED_INFO(std::string(info.name) + ": the record keeps the raw SP and G");
        CHECK((lines.size() == 1 && lines[0].find(" sp_full=0000000100080000 ") != std::string::npos && lines[0].find(" g_full=ABCD000000020000 ") != std::string::npos && lines[0].find(" sp=00080000 ") != std::string::npos));
        // The device is the low word; a different device is not ready.
        auto wrong    = makeEntry(info.id, L0);
        wrong.r31.u64 = L0.g + 16;
        const auto w  = attempt(guest, info.id, wrong);
        UNSCOPED_INFO(std::string(info.name) + ": a device that was not validated is refused");
        CHECK((!w.replaced && w.counts.refusal(R::Services) == 1));
        // The thread register is raw too and compared on every boundary.
        auto thread    = makeEntry(info.id, L0);
        thread.r13.u64 = 0x1234567800000000ULL;
        UNSCOPED_INFO(std::string(info.name) + ": any raw r13 is the thread identity");
        CHECK(attempt(guest, info.id, thread).replaced);
    }
}

void testOriginalRunsOnce()
{
    struct Case
    {
        const char*                              name;
        bool                                     stub, check;
        std::function<void(Guest&, const Info&)> tweak;
        bool                                     consumed;
        int                                      pairing; // 0 ok, 1 unpaired, 2 unreadable
    };

    const Case cases[] = {
        { "consumed", true, false, nullptr, true, 0 },
        { "services refused", true, false, [](Guest& g, const Info&)
          {
              g.graphics = true;
          },
          false,
          1 },
        { "provider refused", true, false, [](Guest& g, const Info& i)
          {
              g.targets.erase(i.importAddress);
          },
          false,
          1 },
        { "stack refused", true, false, [](Guest& g, const Info&)
          {
              g.unreadable.insert(L0.sp + 4);
          },
          false,
          2 },
        { "exit check alone", false, true, nullptr, false, 0 },
        { "both options", true, true, nullptr, false, 0 },
    };
    for (const auto& info : kInfo)
        for (const auto& c : cases)
        {
            const std::string name = std::string(info.name) + ": " + c.name;
            Guest             guest;
            setup(guest, L0);
            guest.install();
            if (c.tweak)
                c.tweak(guest, info);
            Cvars      cvars(info.id, c.stub, c.check);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": consumed exactly when expected");
            CHECK(owner.replaced == c.consumed);
            UNSCOPED_INFO(name + ": the import runs exactly once, or zero times when consumed");
            CHECK(owner.importCalls == (c.consumed ? 0U : 1U));
            if (info.id == kH)
            {
                UNSCOPED_INFO(name + ": the retained child runs once and no diagnostic");
                CHECK((owner.childCalls == 1 && owner.diagCalls == 0));
            }
            else
            {
                UNSCOPED_INFO(name + ": the retained retrain runs once inside the critical region");
                CHECK((owner.retrainCalls == 1 && owner.trace.front() == "KeEnterCriticalRegion" && owner.trace.back() == "KeLeaveCriticalRegion"));
            }
            UNSCOPED_INFO(name + ": enter == consumed + original");
            CHECK((counts.counts.enter == 1 && counts.counts.consumed == (c.consumed ? 1U : 0U) && counts.counts.original == (c.consumed ? 0U : 1U)));
            if (c.check)
            {
                UNSCOPED_INFO(name + ": the exit check wins over consume");
                CHECK(counts.counts.consumed == 0);
            }
            UNSCOPED_INFO(name + ": nothing outstanding");
            CHECK(counts.counts.pending == 0);
            if (c.pairing == 0)
            {
                UNSCOPED_INFO(name + ": the call pairs ok");
                CHECK((counts.counts.exitOk == 1 && counts.quality() == 0));
            }
            else if (c.pairing == 1)
            {
                UNSCOPED_INFO(name + ": the call is observed but never ok");
                CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
            }
            else
            {
                UNSCOPED_INFO(name + ": the call is observed as unreadable");
                CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnreadable == 1));
            }
        }
}

void testServicesAndLease()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        // Unpublished services with the stub on: inactive, nothing is counted or read.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, true, false, false);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": no published services: the import runs");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": unpublished services count and read nothing");
            CHECK((counts.counts.enter == 0 && counts.refusals() == 0 && counts.counts.original == 0 && counts.quality() == 0 && guest.reads == 0 && guest.environments == 0));
        }
        // A withdrawal between the cheap test and the lease runs the import.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, true, false);
            SetRingStubLeaseProbe([]() noexcept
                                  {
                                      rerevved::gpu::guest_d3d::ringStubsActive.store(false, std::memory_order_seq_cst);
                                  });
            const auto owner = runOwner(info.id, guest, L0);
            SetRingStubLeaseProbe(nullptr);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": a withdrawal before the lease runs the import");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": counted as services, not ready");
            CHECK((counts.refusal(R::Services) == 1 && counts.refusals() == 1 && counts.counts.consumed == 0 && counts.counts.original == 1 && counts.counts.readyAtEntry == 0));
            UNSCOPED_INFO(name + ": no environment is read without the lease");
            CHECK(guest.environments == 0);
        }
        // A withdrawal cannot finish while the admitted entry holds its lease, and the entry
        // reads its readiness under that lease.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars              cvars(info.id, true, false);
            std::promise<void> reached, release;
            auto               released = release.get_future().share();
            std::atomic<bool>  withdrawn{ false };
            guest.onEnvironment = [&](unsigned index)
            {
                if (index == 1)
                {
                    reached.set_value();
                    released.wait();
                }
            };
            Owner       owner;
            std::thread writer([&]
                               {
                                   owner = runOwner(info.id, guest, L0);
                               });
            reached.get_future().wait();
            std::thread withdrawer([&]
                                   {
                                       rerevved::gpu::guest_d3d::WithdrawRingStubServices();
                                       withdrawn = true;
                                   });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            UNSCOPED_INFO(name + ": the withdrawal waits for the entry's lease");
            CHECK(!withdrawn.load());
            release.set_value();
            writer.join();
            withdrawer.join();
            UNSCOPED_INFO(name + ": the withdrawal finishes after the entry");
            CHECK(withdrawn.load());
            UNSCOPED_INFO(name + ": readiness is read under the lease after the flag cleared, so the import runs");
            CHECK((!owner.replaced && owner.importCalls == 1));
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": one services refusal, nothing outstanding");
            CHECK((counts.refusal(R::Services) == 1 && counts.refusals() == 1 && counts.counts.consumed == 0 && counts.counts.original == 1 && counts.counts.pending == 0));
        }
        // The lease is still held when the full r3 is assigned.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars             cvars(info.id, true, false);
            std::atomic<bool> withdrawn{ false };
            std::thread       withdrawer;
            guest.onCommit = [&]
            {
                withdrawer = std::thread(
                    [&]
                    {
                        rerevved::gpu::guest_d3d::WithdrawRingStubServices();
                        withdrawn = true;
                    });
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                UNSCOPED_INFO(name + ": a withdrawal cannot finish before the commit");
                CHECK(!withdrawn.load());
            };
            const auto owner = runOwner(info.id, guest, L0);
            withdrawer.join();
            UNSCOPED_INFO(name + ": the consume committed and the withdrawal finished after it");
            CHECK((owner.replaced && withdrawn.load()));
            // No unsupported consume after the withdrawal.
            auto          after = makeEntry(info.id, L0);
            std::uint64_t r3    = after.r3.u64;
            UNSCOPED_INFO(name + ": nothing is consumed after the withdrawal");
            CHECK((!ReplaceVideoImportStubCall(info.callPC, info.id, makeCall(after), r3) && r3 == after.r3.u64));
            takeAll();
        }
        // The lease ends before Original runs: a withdrawal after a refused entry does not wait.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, true, false);
            auto  ctx        = makeEntry(info.id, L0);
            ctx.r30.u64      = info.id == kH ? 0 : ctx.r30.u64;
            ctx.r3.u64       = info.id == kH ? ctx.r3.u64 : 9;
            std::uint64_t r3 = ctx.r3.u64;
            UNSCOPED_INFO(name + ": refused");
            CHECK(!ReplaceVideoImportStubCall(info.callPC, info.id, makeCall(ctx), r3));
            const auto begun = std::chrono::steady_clock::now();
            rerevved::gpu::guest_d3d::WithdrawRingStubServices();
            UNSCOPED_INFO(name + ": the lease was released before the import runs");
            CHECK(std::chrono::steady_clock::now() - begun < std::chrono::seconds(2));
            finishCall(guest, info.id, ctx);
            takeAll();
        }
    }
}

void testDisabledWork()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        PPCContext        ctx  = makeEntry(info.id, L0);
        Guest             guest;
        setup(guest, L0);
        guest.install();
        for (int mode = 0; mode < 2; ++mode)
        {
            // Mode 0: both options off with services published. Mode 1: the stub on, services not published.
            Cvars         cvars(info.id, mode == 1, false, mode == 0);
            const auto    touches   = VideoImportThreadTouchesForTesting();
            bool          replaced  = false;
            std::uint64_t allocated = 0;
            {
                rerevved::tests::allocation::ScopedAllocationCount count;
                for (int i = 0; i < 1000; ++i)
                {
                    replaced |= hookReplace(info.id, ctx);
                    replaced |= ReplaceVideoImportStubCall(info.callPC, info.id, makeCall(ctx), ctx.r3.u64);
                    hookReturn(info.id, ctx);
                    hookEffect(info.id, ctx);
                    hookPost(info.id, ctx);
                }
                allocated = count.Count();
            }
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": disabled: nothing is replaced");
            CHECK(!replaced);
            UNSCOPED_INFO(name + ": disabled: no allocation");
            CHECK(allocated == 0);
            UNSCOPED_INFO(name + ": disabled: no memory access and no admission callback");
            CHECK((guest.reads == 0 && guest.environments == 0));
            UNSCOPED_INFO(name + ": disabled: no thread bookkeeping");
            CHECK(VideoImportThreadTouchesForTesting() == touches);
            UNSCOPED_INFO(name + ": disabled: nothing is counted");
            CHECK((counts.counts.enter == 0 && counts.counts.readyAtEntry == 0 && counts.refusals() == 0 && counts.counts.original == 0 && counts.counts.exitOk == 0 && counts.counts.pending == 0 && counts.quality() == 0));
            if (mode == 0)
            {
                UNSCOPED_INFO(name + ": disabled: the drains return no fields");
                CHECK((ReportVideoImportStubs().empty() && ReportVideoImportStubs(true).empty() && ReportVideoHsioStub().empty() && ReportVideoEdramWorkerStub(true).empty()));
            }
            UNSCOPED_INFO(name + ": disabled: nothing is logged");
            CHECK(guest.logLines().empty());
            UNSCOPED_INFO(name + ": disabled: registers unchanged");
            CHECK(sameContext(ctx, makeEntry(info.id, L0)));
        }
    }
}

// Runs one fault through the boundaries of a call that the exit check observes.
struct Fault
{
    const char*                              name;
    Id                                       id;
    std::function<void(Guest&, PPCContext&)> ret, eff, post;
    std::function<void(Guest&)>              beforeEffect;
    int                                      kind; // a mismatch kind, or -1
    bool                                     unreadable = false;
};

// The fault under the caller's configuration.
Counts runFaultConfigured(const Fault& f, Guest& guest)
{
    auto entry = makeEntry(f.id, L0);
    auto copy  = entry;
    (void)hookReplace(f.id, copy);
    auto ctx = atReturn(f.id, entry);
    if (f.ret)
        f.ret(guest, ctx);
    hookReturn(f.id, ctx);
    owedStores(guest, f.id, L0);
    if (f.beforeEffect)
        f.beforeEffect(guest);
    ctx = atEffect(f.id, entry);
    if (f.eff)
        f.eff(guest, ctx);
    hookEffect(f.id, ctx);
    ctx = atPost(f.id, entry);
    if (f.post)
        f.post(guest, ctx);
    hookPost(f.id, ctx);
    return take(f.id);
}

Counts runFault(const Fault& f, Guest& guest)
{
    Cvars cvars(f.id, false, true);
    return runFaultConfigured(f, guest);
}

void testExitCheck()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        // Both options: the import is forced to run and the call is observed.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, true, true);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": exit check: the import runs exactly once");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": exit check: consumed 0, one diagnostics refusal");
            CHECK((counts.counts.consumed == 0 && counts.counts.original == 1 && counts.refusal(R::Diagnostics) == 1 && counts.refusals() == 1));
            UNSCOPED_INFO(name + ": exit check: the matched observation is ok");
            CHECK((counts.counts.exitOk == 1 && counts.quality() == 0));
        }
        // Both options with a failing earlier prerequisite count that prerequisite, not diagnostics.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            guest.targets.erase(info.importAddress);
            Cvars      cvars(info.id, true, true);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": exit check: the provider refusal wins over diagnostics");
            CHECK((!owner.replaced && counts.refusal(R::Provider) == 1 && counts.refusal(R::Diagnostics) == 0 && counts.counts.consumed == 0));
            UNSCOPED_INFO(name + ": an invalid provider is never ok");
            CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired >= 1));
        }
        // The exit check alone observes without any refusal, with the lease and probe of every active entry.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, false, true);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": check only: the import runs");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": check only: exit_ok");
            CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.refusals() == 0 && counts.counts.exitOk == 1));
            UNSCOPED_INFO(name + ": check only: nothing outstanding, ready at entry");
            CHECK((counts.counts.pending == 0 && counts.counts.consumed == 0 && counts.counts.readyAtEntry == 1));
            UNSCOPED_INFO(name + ": check only: the identities are read at the entry and at each later boundary");
            CHECK(guest.environments == 4);
            UNSCOPED_INFO(name + ": check only: the probe reads the row's own import address");
            CHECK((!guest.probedAddresses.empty() && std::all_of(guest.probedAddresses.begin(), guest.probedAddresses.end(), [&](std::uint32_t a)
                                                                 {
                                                                     return a == info.importAddress;
                                                                 })));
        }
        // Without a held lease the provider is not valid: the call is observed but never ok.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, false, true, false);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": check only without services: the import runs");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": check only without services: no refusal, not ready");
            CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.refusals() == 0 && counts.counts.readyAtEntry == 0));
            UNSCOPED_INFO(name + ": check only without services: provider_valid is false, so unpaired");
            CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.counts.pending == 0));
            const auto logs = guest.logLines();
            UNSCOPED_INFO(name + ": the record says the provider was not valid");
            CHECK((logs.size() == 1 && logs[0].find(" provider_valid=0 ") != std::string::npos && logs[0].find(" ready_at_entry=0 ") != std::string::npos && logs[0].find("verdict=unpaired") != std::string::npos));
        }
        // A graphics system, a missing delegate or a missing ring service make an observed call inconclusive.
        for (int variant = 0; variant < 3; ++variant)
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            if (variant == 0)
                guest.graphics = true;
            else if (variant == 1)
                guest.delegate = false;
            else
                guest.ring = false;
            Cvars cvars(info.id, false, true);
            (void)runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": an invalid route is never ok");
            CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
        }
        // The readiness is recorded separately from the observation: a device that was not validated.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, false, true, true, L0.g + 4);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": not ready: one Original");
            CHECK((!owner.replaced && owner.importCalls == 1));
            UNSCOPED_INFO(name + ": not ready: enter counted, ready_at_entry not");
            CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.counts.readyAtEntry == 0 && counts.refusals() == 0));
            const auto logs = guest.logLines();
            UNSCOPED_INFO(name + ": not ready: the record says so");
            CHECK((logs.size() == 1 && logs[0].find(" ready_at_entry=0 ") != std::string::npos));
        }
        // The stub on a device that was not validated: a services refusal, one Original, not ready.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, true, false, true, L0.g + 4);
            const auto owner  = runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": the stub on an unready device is a services refusal");
            CHECK((!owner.replaced && counts.refusal(R::Services) == 1 && counts.counts.readyAtEntry == 0 && counts.counts.original == 1));
        }
    }

    const Fault faults[] = {
        { "hsio wrong result", kH, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0;
          },
          nullptr,
          nullptr,
          nullptr,
          static_cast<int>(M::Return) },
        { "hsio wrong result in the high word", kH, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0x0000000100000001ULL;
          },
          nullptr,
          nullptr,
          nullptr,
          static_cast<int>(M::Return) },
        { "hsio r11 not zero", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.r11.u64 = 5;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio r11 high word", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.r11.u64 = 0x0000000100000000ULL;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio CR0 greater", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.cr0.set_raw(0x4);
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio CR0 less", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.cr0.set_raw(0x8);
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio CR0 not equal", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.cr0.set_raw(0x0);
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio CR0 equal and less", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.cr0.set_raw(0xA);
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Effect) },
        { "hsio cache not zero", kH, nullptr, nullptr, nullptr, [](Guest& g)
          {
              g.put32(kCache, 1);
          },
          static_cast<int>(M::Effect) },
        { "hsio retained r30 changed", kH, nullptr, nullptr, [](Guest&, PPCContext& c)
          {
              c.r30.u64 ^= 1;
          },
          nullptr,
          static_cast<int>(M::Continuation) },
        { "hsio summary overflow retained is not a mismatch", kH, nullptr, [](Guest&, PPCContext& c)
          {
              c.cr0.set_raw(0x3);
          },
          nullptr,
          nullptr,
          -1 },
        { "hsio child result zero is not a mismatch", kH, nullptr, nullptr, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0;
          },
          nullptr,
          -1 },
        { "hsio child result nonzero is not a mismatch", kH, nullptr, nullptr, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0x1234567800000007ULL;
          },
          nullptr,
          -1 },
        { "hsio cache unreadable at the effect", kH, nullptr, nullptr, nullptr, [](Guest& g)
          {
              g.unreadable.insert(kCache);
          },
          -1,
          true },
        { "worker wrong result", kW, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 1;
          },
          nullptr,
          nullptr,
          nullptr,
          static_cast<int>(M::Return) },
        { "worker wrong result in the high word", kW, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0x0000000100000000ULL;
          },
          nullptr,
          nullptr,
          nullptr,
          static_cast<int>(M::Return) },
        { "worker r3 is not the saved r4", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r3.u64 ^= 4;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r4 is not SP+80", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r4.u64 += 4;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r5 is not 4096", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r5.u64 = 4095;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r6 is not SP+84", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r6.u64 += 4;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r7 is not SP+96", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r7.u64 += 4;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r8 is not 2048", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r8.u64 = 2047;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r29 changed", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r29.u64 += 1;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r30 changed", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r30.u64 += 1;
              c.r3.u64 = c.r30.u64;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker r30 changed alone", kW, nullptr, [](Guest&, PPCContext& c)
          {
              c.r30.u64 += 1;
          },
          nullptr,
          nullptr,
          static_cast<int>(M::Continuation) },
        { "worker stack word 80 not zero", kW, nullptr, nullptr, nullptr, [](Guest& g)
          {
              g.put32(L0.sp + 80, 1);
          },
          static_cast<int>(M::Effect) },
        { "worker stack word 84 not zero", kW, nullptr, nullptr, nullptr, [](Guest& g)
          {
              g.put32(L0.sp + 84, 1);
          },
          static_cast<int>(M::Effect) },
        { "worker LR after the retrain", kW, nullptr, nullptr, [](Guest&, PPCContext& c)
          {
              c.lr = 0x826B056C;
          },
          nullptr,
          static_cast<int>(M::Lr) },
        { "worker nonzero retrain result is not a mismatch", kW, nullptr, nullptr, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0xFFFFFFFF00000005ULL;
          },
          nullptr,
          -1 },
        { "worker pre-retrain stack word unreadable", kW, nullptr, nullptr, nullptr, [](Guest& g)
          {
              g.unreadable.insert(L0.sp + 84);
          },
          -1,
          true },
        { "worker post-retrain stack word 84 unreadable", kW, nullptr, nullptr, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp + 84);
          },
          nullptr,
          -1,
          true },
        { "worker post-retrain stack word 80 unreadable", kW, nullptr, nullptr, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp + 80);
          },
          nullptr,
          -1,
          true },
    };
    for (const auto& f : faults)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        const auto        counts = runFault(f, guest);
        const std::string name   = f.name;
        if (f.unreadable)
        {
            UNSCOPED_INFO(name + ": an unreadable word is inconclusive");
            CHECK((counts.counts.exitUnreadable == 1 && counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 0));
            const auto logs = guest.logLines();
            UNSCOPED_INFO(name + ": the record says unreadable");
            CHECK((logs.size() == 1 && logs[0].find("verdict=unreadable") != std::string::npos));
        }
        else if (f.kind < 0)
        {
            UNSCOPED_INFO(name + ": ok, not a mismatch");
            CHECK((counts.counts.exitOk == 1 && counts.quality() == 0));
        }
        else
        {
            const auto kind = static_cast<M>(f.kind);
            UNSCOPED_INFO(name + ": counted as that mismatch and not ok");
            CHECK((counts.mismatch(kind) == 1 && counts.mismatches() == 1 && counts.counts.exitOk == 0 && counts.counts.exitUnreadable == 0 && counts.counts.exitUnpaired == 0));
        }
    }

    // An unreadable entry word is inconclusive too: the frame and the HSIO cache.
    for (const auto& info : kInfo)
        for (const bool cache : { false, true })
        {
            if (cache && info.id == kW)
                continue;
            Guest guest;
            setup(guest, L0);
            guest.install();
            guest.unreadable.insert(cache ? kCache : L0.sp + info.frameBytes - 4);
            Cvars cvars(info.id, false, true);
            (void)runOwner(info.id, guest, L0);
            const auto counts = take(info.id);
            UNSCOPED_INFO(std::string(info.name) + ": an unreadable entry word is exit_unreadable");
            CHECK((counts.counts.exitUnreadable == 1 && counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 0 && counts.counts.pending == 0));
        }
}

// ---------------------------------------------------------------------------

void addCounts(Counts& total, const Counts& more)
{
    auto&       t = total.counts;
    const auto& m = more.counts;
    t.enter += m.enter;
    t.readyAtEntry += m.readyAtEntry;
    t.consumed += m.consumed;
    t.original += m.original;
    for (std::size_t i = 0; i < t.refusal.size(); ++i)
        t.refusal[i] += m.refusal[i];
    t.exitOk += m.exitOk;
    for (std::size_t i = 0; i < t.mismatch.size(); ++i)
        t.mismatch[i] += m.mismatch[i];
    t.exitUnreadable += m.exitUnreadable;
    t.exitUnpaired += m.exitUnpaired;
    t.reentry += m.reentry;
    t.abandoned += m.abandoned;
    t.correlationLost += m.correlationLost;
    t.reportingFailed += m.reportingFailed;
    t.pending = m.pending;
}

void testIdentityChanges()
{
    struct Change
    {
        const char*                              name;
        std::function<void(Guest&, const Info&)> apply;
    };

    const Change changes[] = {
        { "runtime identity", [](Guest& g, const Info&)
          {
              g.runtimeId += 1;
          } },
        { "delegate identity", [](Guest& g, const Info&)
          {
              g.delegateId += 1;
          } },
        { "ring identity", [](Guest& g, const Info&)
          {
              g.ringId += 1;
          } },
        { "provider target identity", [](Guest& g, const Info&)
          {
              g.targetBias = 8;
          } },
        { "provider target replaced by the other wrapper", [](Guest& g, const Info& i)
          {
              g.targets[i.importAddress] = i.id == kH ? &__imp__VdRetrainEDRAMWorker : &__imp__VdIsHSIOTrainingSucceeded;
          } },
        { "delegate object replaced at the same integer identity", [](Guest& g, const Info&)
          {
              g.delegateObject = std::make_shared<int>(9);
          } },
        { "ring object replaced at the same integer identity", [](Guest& g, const Info&)
          {
              g.ringObject = std::make_shared<int>(9);
          } },
        { "delegate object expired", [](Guest& g, const Info&)
          {
              g.delegateObject.reset();
          } },
        { "ring object expired", [](Guest& g, const Info&)
          {
              g.ringObject.reset();
          } },
        { "delegate withdrawn", [](Guest& g, const Info&)
          {
              g.delegate = false;
          } },
        { "ring service withdrawn", [](Guest& g, const Info&)
          {
              g.ring = false;
          } },
        { "ring stub publication withdrawn", [](Guest&, const Info&)
          {
              rerevved::gpu::guest_d3d::WithdrawRingStubServices();
          } },
        { "ring stubs disabled for the session", [](Guest&, const Info&)
          {
              // Publishing nothing clears the sticky disable of an earlier case; the flag is then set again by hand.
              rerevved::gpu::guest_d3d::PublishRingStubServices(nullptr);
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(L0.g);
              rerevved::gpu::guest_d3d::ringStubsActive.store(true);
              rerevved::gpu::guest_d3d::DisableRingStubs("test");
          } },
        { "validated device cleared", [](Guest&, const Info&)
          {
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
          } },
        { "validated device replaced", [](Guest&, const Info&)
          {
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(L0.g + 4);
          } },
    };
    const char* const boundaries[] = { "return", "effect", "post-child" };
    for (const auto& info : kInfo)
        for (const auto& change : changes)
            for (int boundary = 0; boundary < 3; ++boundary)
            {
                const std::string name = std::string(info.name) + ": " + change.name + " before the " + boundaries[boundary] + " observer";
                Guest             guest;
                setup(guest, L0);
                guest.install();
                Cvars      cvars(info.id, false, true);
                Opts       opts;
                const auto apply = [&]
                {
                    change.apply(guest, info);
                };
                (boundary == 0 ? opts.pauseReturn : boundary == 1 ? opts.pauseEffect
                                                                  : opts.pausePost) = apply;
                (void)runOwner(info.id, guest, L0, opts);
                const auto counts = take(info.id);
                UNSCOPED_INFO(name + ": exit_unpaired, never ok");
                CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.mismatches() == 0 && counts.counts.exitUnreadable == 0));
                UNSCOPED_INFO(name + ": the record closes");
                CHECK((counts.counts.correlationLost == 0 && counts.counts.pending == 0));
                const auto logs = guest.logLines();
                UNSCOPED_INFO(name + ": the record says the identity changed");
                CHECK((logs.size() == 1 && logs[0].find(" identity_changed=1 ") != std::string::npos && logs[0].find(" provider_valid=0 ") != std::string::npos && logs[0].find("verdict=unpaired") != std::string::npos));
            }

    // A call that was not ready at entry still depends on the publication it ran under: a withdrawal at any
    // boundary is unpaired, and the same call untouched is ok.
    for (const auto& info : kInfo)
        for (int boundary = -1; boundary < 3; ++boundary)
        {
            const std::string name = std::string(info.name) + ": a not-ready call " + (boundary < 0 ? "left alone" : std::string("withdrawn before the ") + boundaries[boundary] + " observer");
            Guest             guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true, true, L0.g + 4);
            Opts  opts;
            if (boundary >= 0)
                (boundary == 0 ? opts.pauseReturn : boundary == 1 ? opts.pauseEffect
                                                                  : opts.pausePost) = []
                {
                    rerevved::gpu::guest_d3d::WithdrawRingStubServices();
                };
            (void)runOwner(info.id, guest, L0, opts);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": not ready at entry, and the record closes");
            CHECK((counts.counts.readyAtEntry == 0 && counts.counts.pending == 0 && counts.mismatches() == 0));
            if (boundary < 0)
            {
                UNSCOPED_INFO(name + ": exit_ok");
                CHECK((counts.counts.exitOk == 1 && counts.counts.exitUnpaired == 0));
            }
            else
            {
                UNSCOPED_INFO(name + ": exit_unpaired, never ok");
                CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
            }
        }

    // An identity that changes at one boundary and is restored before the next still invalidates the pair.
    for (const auto& info : kInfo)
        for (int boundary = 0; boundary < 2; ++boundary)
        {
            const std::string name = std::string(info.name) + ": a ring identity changed and restored around the " + (boundary == 0 ? "return" : "effect") + " observer";
            Guest             guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            Opts  opts;
            (boundary == 0 ? opts.pauseReturn : opts.pauseEffect) = [&]
            {
                guest.ringId += 1;
            };
            (boundary == 0 ? opts.pauseEffect : opts.pausePost) = [&]
            {
                guest.ringId -= 1;
            };
            (void)runOwner(info.id, guest, L0, opts);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": exit_unpaired, never ok");
            CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.mismatches() == 0 && counts.counts.pending == 0));
        }

    // An entry without weak ownership can never be exit_ok.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        guest.ownersBound = false;
        Cvars cvars(info.id, false, true);
        (void)runOwner(info.id, guest, L0);
        const auto counts = take(info.id);
        UNSCOPED_INFO(std::string(info.name) + ": an unbound weak ownership is never ok");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
    }

    // The record holds no strong reference to the delegate or the ring service.
    for (const auto& info : kInfo)
    {
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            Opts  opts;
            long  delegateUses = -1, ringUses = -1;
            opts.pauseEffect = [&]
            {
                delegateUses = guest.delegateObject.use_count();
                ringUses     = guest.ringObject.use_count();
            };
            (void)runOwner(info.id, guest, L0, opts);
            const auto counts = take(info.id);
            UNSCOPED_INFO(std::string(info.name) + ": a live record holds no strong ownership");
            CHECK((delegateUses == 1 && ringUses == 1));
            UNSCOPED_INFO(std::string(info.name) + ": and the pair is ok");
            CHECK(counts.counts.exitOk == 1);
        }
        // Releasing the external owners while a record is live expires the binding.
        {
            Guest released;
            setup(released, L0);
            released.install();
            Cvars              cvars(info.id, false, true);
            std::weak_ptr<int> delegateWeak = released.delegateObject, ringWeak = released.ringObject;
            Opts               second;
            second.pauseEffect = [&]
            {
                released.delegateObject.reset();
                released.ringObject.reset();
            };
            (void)runOwner(info.id, released, L0, second);
            UNSCOPED_INFO(std::string(info.name) + ": the record did not keep the owners alive");
            CHECK((delegateWeak.expired() && ringWeak.expired()));
            const auto after = take(info.id);
            UNSCOPED_INFO(std::string(info.name) + ": expired bindings are exit_unpaired");
            CHECK((after.counts.exitOk == 0 && after.counts.exitUnpaired == 1));
        }
    }
}

void testObservationPairing()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        const auto        run  = [&](Opts opts)
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            (void)runOwner(info.id, guest, L0, opts);
            return take(info.id);
        };
        {
            const auto c = run(Opts{});
            UNSCOPED_INFO(name + ": a complete call pairs ok");
            CHECK((c.counts.exitOk == 1 && c.quality() == 0 && c.counts.pending == 0));
        }
        {
            Opts opts;
            opts.skipReturn = true;
            const auto c    = run(opts);
            UNSCOPED_INFO(name + ": a lost return is two unpaired boundaries, never ok");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 2 && c.counts.correlationLost == 2 && c.counts.pending == 0));
        }
        {
            Opts opts;
            opts.dupReturn = true;
            const auto c   = run(opts);
            UNSCOPED_INFO(name + ": a duplicated return is unpaired, never ok");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 2 && c.counts.correlationLost == 1 && c.counts.pending == 0));
        }
        {
            Opts opts;
            opts.skipEffect = true;
            const auto c    = run(opts);
            UNSCOPED_INFO(name + ": a lost effect is unpaired, never ok");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 1 && c.counts.correlationLost == 1 && c.counts.pending == 0));
        }
        {
            // The post-child observer lost: the record stays until the next entry abandons it.
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            Opts  opts;
            opts.skipPost = true;
            (void)runOwner(info.id, guest, L0, opts);
            const auto first = take(info.id);
            UNSCOPED_INFO(name + ": a lost post leaves the call outstanding");
            CHECK((first.counts.exitOk == 0 && first.counts.pending == 1));
            (void)runOwner(info.id, guest, L0);
            const auto second = take(info.id);
            UNSCOPED_INFO(name + ": the next entry abandons it and pairs its own call");
            CHECK((second.counts.abandoned == 1 && second.counts.exitUnpaired == 1 && second.counts.exitOk == 1 && second.counts.pending == 0));
        }
        {
            // A higher SP is also a new frame: the earlier one is gone.
            Guest guest;
            setup(guest, L0);
            setup(guest, L1);
            guest.install();
            Cvars cvars(info.id, false, true);
            Opts  opts;
            opts.skipPost = true;
            (void)runOwner(info.id, guest, L0, opts);
            (void)take(info.id);
            (void)runOwner(info.id, guest, L1);
            const auto second = take(info.id);
            UNSCOPED_INFO(name + ": a higher frame abandons the stale record");
            CHECK((second.counts.abandoned == 1 && second.counts.exitUnpaired == 1 && second.counts.exitOk == 1 && second.counts.pending == 0));
        }
        {
            // SP reuse after a finished call is not an abandonment.
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            for (int i = 0; i < 3; ++i)
                (void)runOwner(info.id, guest, L0);
            const auto c = take(info.id);
            UNSCOPED_INFO(name + ": a reused SP after finished calls pairs ok");
            CHECK((c.counts.exitOk == 3 && c.counts.abandoned == 0 && c.quality() == 0));
        }
        {
            // Drains between every boundary preserve the record and the gauge.
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars         cvars(info.id, false, true);
            Counts        total;
            Opts          opts;
            std::uint64_t seenPending[3]{};
            opts.pauseReturn = [&]
            {
                const auto c   = take(info.id);
                seenPending[0] = c.counts.pending;
                addCounts(total, c);
            };
            opts.pauseEffect = [&]
            {
                const auto c   = take(info.id);
                seenPending[1] = c.counts.pending;
                addCounts(total, c);
            };
            opts.pausePost = [&]
            {
                const auto c   = take(info.id);
                seenPending[2] = c.counts.pending;
                addCounts(total, c);
            };
            (void)runOwner(info.id, guest, L0, opts);
            addCounts(total, take(info.id));
            UNSCOPED_INFO(name + ": a drain keeps the outstanding gauge at every boundary");
            CHECK((seenPending[0] == 1 && seenPending[1] == 1 && seenPending[2] == 1));
            UNSCOPED_INFO(name + ": the pair completes across drains");
            CHECK((total.counts.enter == 1 && total.counts.exitOk == 1 && total.quality() == 0 && total.counts.pending == 0));
        }
        {
            // A withdrawal between the boundaries breaks the publication the call relied on, so the pair is unpaired.
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, true, false);
            Opts  opts;
            opts.pauseEffect = [&]
            {
                rerevved::gpu::guest_d3d::WithdrawRingStubServices();
            };
            const auto owner = runOwner(info.id, guest, L0, opts);
            const auto c     = take(info.id);
            UNSCOPED_INFO(name + ": a withdrawal between the boundaries is unpaired, never ok");
            CHECK((owner.replaced && c.counts.consumed == 1 && c.counts.exitOk == 0 && c.counts.exitUnpaired == 1 && c.counts.pending == 0));
        }
        {
            // Inactive with no live record: no work at all.
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars      cvars(info.id, true, false, false);
            const auto touches = VideoImportThreadTouchesForTesting();
            finishCall(guest, info.id, makeEntry(info.id, L0));
            UNSCOPED_INFO(name + ": inactive observers with no live record do no work");
            CHECK((VideoImportThreadTouchesForTesting() == touches && take(info.id).quality() == 0));
        }

        // Mismatched registers poison the pair at every boundary, whatever register differs.
        struct Skew
        {
            const char*                      name;
            std::function<void(PPCContext&)> apply;
        };

        const Skew skews[] = {
            { "SP low word", [](PPCContext& c)
              {
                  c.r1.u64 += 16;
              } },
            { "SP high word", [](PPCContext& c)
              {
                  c.r1.u64 |= 0x0000000100000000ULL;
              } },
            { "device", [](PPCContext& c)
              {
                  c.r31.u64 += 16;
              } },
            { "device high word", [](PPCContext& c)
              {
                  c.r31.u64 |= 0x0000000100000000ULL;
              } },
            { "guest thread", [](PPCContext& c)
              {
                  c.r13.u64 += 0x100;
              } },
            { "guest thread high word", [](PPCContext& c)
              {
                  c.r13.u64 ^= 0x0100000000000000ULL;
              } },
        };
        for (const auto& skew : skews)
            for (int boundary = 0; boundary < 3; ++boundary)
            {
                const std::string label = name + ": " + skew.name + " differs at boundary " + std::to_string(boundary);
                Guest             guest;
                setup(guest, L0);
                guest.install();
                Cvars cvars(info.id, false, true);
                auto  entry = makeEntry(info.id, L0);
                auto  copy  = entry;
                (void)hookReplace(info.id, copy);
                owedStores(guest, info.id, L0);
                auto ret = atReturn(info.id, entry);
                if (boundary == 0)
                    skew.apply(ret);
                hookReturn(info.id, ret);
                auto eff = atEffect(info.id, entry);
                if (boundary == 1)
                    skew.apply(eff);
                hookEffect(info.id, eff);
                auto post = atPost(info.id, entry);
                if (boundary == 2)
                    skew.apply(post);
                hookPost(info.id, post);
                const auto c = take(info.id);
                UNSCOPED_INFO(label + ": correlation lost, never ok");
                CHECK((c.counts.exitOk == 0 && c.counts.correlationLost >= 1 && c.counts.exitUnpaired >= 1));
                if (boundary == 2)
                {
                    UNSCOPED_INFO(label + ": the call stays outstanding");
                    CHECK(c.counts.pending == 1);
                    // The next entry abandons it and pairs its own call.
                    auto next = entry;
                    (void)hookReplace(info.id, next);
                    finishCall(guest, info.id, entry);
                    const auto after = take(info.id);
                    UNSCOPED_INFO(label + ": the next entry abandons the stale record");
                    CHECK((after.counts.abandoned == 1 && after.counts.exitOk == 1 && after.counts.pending == 0));
                }
                else
                {
                    UNSCOPED_INFO(label + ": the record closes");
                    CHECK(c.counts.pending == 0);
                }
            }
        // Reordered boundaries: effect, return, post.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            auto  entry = makeEntry(info.id, L0);
            auto  copy  = entry;
            (void)hookReplace(info.id, copy);
            owedStores(guest, info.id, L0);
            auto eff = atEffect(info.id, entry);
            hookEffect(info.id, eff);
            auto ret = atReturn(info.id, entry);
            hookReturn(info.id, ret);
            auto post = atPost(info.id, entry);
            hookPost(info.id, post);
            const auto c = take(info.id);
            UNSCOPED_INFO(name + ": reordered boundaries are never ok");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 2 && c.counts.correlationLost == 2 && c.counts.pending == 0));
        }
        // The post-child observer before the effect.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, false, true);
            auto  entry = makeEntry(info.id, L0);
            auto  copy  = entry;
            (void)hookReplace(info.id, copy);
            owedStores(guest, info.id, L0);
            auto ret = atReturn(info.id, entry);
            hookReturn(info.id, ret);
            auto post = atPost(info.id, entry);
            hookPost(info.id, post);
            const auto c = take(info.id);
            UNSCOPED_INFO(name + ": a post before the effect is never ok");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 1 && c.counts.correlationLost == 1 && c.counts.pending == 0));
        }
        // A nested call below the live frame is refused and poisons the outer pair.
        {
            Guest guest;
            setup(guest, L0);
            setup(guest, L2);
            guest.install();
            Cvars cvars(info.id, true, false);
            auto  outer = makeEntry(info.id, L0);
            auto  inner = makeEntry(info.id, L2);
            auto  a     = outer;
            auto  b     = inner;
            UNSCOPED_INFO(name + ": the outer call is consumed");
            CHECK(hookReplace(info.id, a));
            UNSCOPED_INFO(name + ": the nested call is refused and runs the import");
            CHECK((!hookReplace(info.id, b) && sameContext(b, inner)));
            finishCall(guest, info.id, inner);
            finishCall(guest, info.id, outer);
            const auto c = take(info.id);
            UNSCOPED_INFO(name + ": one reentrant refusal");
            CHECK((c.counts.enter == 2 && c.counts.consumed == 1 && c.counts.original == 1 && c.refusal(R::Reentrant) == 1 && c.refusals() == 1 && c.counts.reentry == 1));
            UNSCOPED_INFO(name + ": neither nested pair is ok and nothing stays outstanding");
            CHECK((c.counts.exitOk == 0 && c.counts.exitUnpaired == 4 && c.counts.correlationLost == 3 && c.counts.pending == 0));
        }
        // Two threads interleave their boundaries without sharing a record.
        {
            Guest guest;
            setup(guest, L0);
            setup(guest, L1);
            guest.install();
            Cvars              cvars(info.id, true, false);
            std::promise<void> reachedA, reachedB, goA, goB;
            auto               goAFuture = goA.get_future().share();
            auto               goBFuture = goB.get_future().share();
            bool               replacedA = false, replacedB = false;
            const auto         worker = [&](const Layout& l, std::promise<void>& reached, std::shared_future<void> go, bool& replaced)
            {
                auto entry = makeEntry(info.id, l);
                auto ctx   = entry;
                replaced   = hookReplace(info.id, ctx);
                reached.set_value();
                go.wait();
                finishCall(guest, info.id, entry);
            };
            std::thread a(worker, std::cref(L0), std::ref(reachedA), goAFuture, std::ref(replacedA));
            std::thread b(worker, std::cref(L1), std::ref(reachedB), goBFuture, std::ref(replacedB));
            reachedA.get_future().wait();
            reachedB.get_future().wait();
            const auto peak = take(info.id);
            UNSCOPED_INFO(name + ": two threads hold two records");
            CHECK((peak.counts.pending == 2 && peak.counts.consumed == 2));
            goA.set_value();
            a.join();
            const auto mid = take(info.id);
            UNSCOPED_INFO(name + ": one finished, one outstanding");
            CHECK(mid.counts.pending == 1);
            goB.set_value();
            b.join();
            const auto last = take(info.id);
            UNSCOPED_INFO(name + ": both pairs are ok");
            CHECK((replacedA && replacedB && last.counts.pending == 0 && mid.counts.exitOk + last.counts.exitOk == 2 && mid.quality() + last.quality() == 0));
        }
        // The pending bound, with a record per thread.
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars cvars(info.id, true, false);
            SetVideoImportPendingLimitForTesting(info.id, 2);
            std::promise<void>              go;
            auto                            goFuture = go.get_future().share();
            std::vector<std::thread>        threads;
            std::vector<std::promise<void>> reached(3);
            std::atomic<unsigned>           replaced{ 0 };
            for (std::size_t i = 0; i < 3; ++i)
            {
                threads.emplace_back([&, i]
                                     {
                                         auto entry = makeEntry(info.id, L0);
                                         auto ctx   = entry;
                                         if (hookReplace(info.id, ctx))
                                             replaced.fetch_add(1);
                                         reached[i].set_value();
                                         goFuture.wait();
                                         finishCall(guest, info.id, entry);
                                     });
                reached[i].get_future().wait();
            }
            const auto c = take(info.id);
            UNSCOPED_INFO(name + ": the third call is refused at the bound");
            CHECK((c.counts.pending == 2 && c.counts.consumed == 2 && replaced.load() == 2 && c.refusal(R::Unpaired) == 1 && c.refusals() == 1 && c.counts.original == 1));
            go.set_value();
            for (auto& t : threads)
                t.join();
            const auto last = take(info.id);
            UNSCOPED_INFO(name + ": the two admitted pairs finish ok");
            CHECK((last.counts.pending == 0 && last.counts.exitOk == 2));
            SetVideoImportPendingLimitForTesting(info.id, kVideoImportMaxPending);
        }
    }

    // Two reservations race at the limit of one: exactly one wins.
    for (const auto& info : kInfo)
    {
        static std::atomic<int>  probeEntered;
        static std::atomic<bool> probeRelease;
        probeEntered = 0;
        probeRelease = false;
        Guest guest;
        setup(guest, L0);
        setup(guest, L1);
        guest.install();
        Cvars cvars(info.id, true, false);
        SetVideoImportPendingLimitForTesting(info.id, 1);
        SetVideoImportReserveProbeForTesting(
            []()
            {
                if (probeEntered.fetch_add(1) == 0)
                    while (!probeRelease.load())
                        std::this_thread::yield();
            });
        bool        replacedA = false;
        std::thread a(
            [&]
            {
                auto ctx  = makeEntry(info.id, L0);
                replacedA = hookReplace(info.id, ctx);
            });
        while (probeEntered.load() < 1)
            std::this_thread::yield();
        auto       entry     = makeEntry(info.id, L1);
        auto       ctx       = entry;
        const bool replacedB = hookReplace(info.id, ctx);
        probeRelease         = true;
        a.join();
        const auto c = take(info.id);
        UNSCOPED_INFO(std::string(info.name) + ": the reservation made while the other waited wins");
        CHECK((replacedB && !replacedA));
        UNSCOPED_INFO(std::string(info.name) + ": exactly one reservation wins at the limit");
        CHECK((c.counts.pending == 1 && c.counts.consumed == 1 && c.refusal(R::Unpaired) == 1 && c.refusals() == 1));
        finishCall(guest, info.id, entry);
        takeAll();
        SetVideoImportReserveProbeForTesting(nullptr);
    }
}

void testBypass()
{
    // HSIO: the cached-nonzero path reaches the effect and post observers with no call.
    for (const bool stub : { true, false })
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars     cvars(kH, stub, true);
        HsioModel model;
        model.cache      = 1;
        const auto owner = runHsioOwner(guest, L0, Opts{}, model);
        const auto c     = take(kH);
        UNSCOPED_INFO("hsio: the cached path runs its diagnostics and child");
        CHECK((owner.path == "cached" && owner.importCalls == 0 && owner.diagCalls == 1 && owner.childCalls == 1));
        UNSCOPED_INFO("hsio: the cached-nonzero bypass counts no selected work");
        CHECK((c.counts.enter == 0 && c.counts.pending == 0 && c.quality() == 0 && c.counts.exitOk == 0 && c.refusals() == 0));
        UNSCOPED_INFO("hsio: the bypass probes and logs nothing");
        CHECK((guest.environments == 0 && guest.logLines().empty()));
    }
    // HSIO: a cached bypass nested under a live record poisons that record.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L2);
        guest.install();
        Cvars cvars(kH, false, true);
        auto  outer = makeEntry(kH, L0);
        auto  copy  = outer;
        (void)hookReplace(kH, copy);
        HsioModel model;
        model.cache = 1;
        (void)runHsioOwner(guest, L2, Opts{}, model);
        finishCall(guest, kH, outer);
        const auto c = take(kH);
        UNSCOPED_INFO("hsio: a nested bypass makes the outer pair inconclusive");
        CHECK((c.counts.exitOk == 0 && c.counts.pending == 0 && c.counts.correlationLost >= 1));
    }
    // Worker: a zero or high-only r29 is the owner's bypass and does no selected work.
    for (const std::uint64_t r29 : std::initializer_list<std::uint64_t>{ std::uint64_t{ 0 }, 0xFFFFFFFF00000000ULL, 0x0000000100000000ULL })
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars       cvars(kW, true, true);
        WorkerModel model;
        model.r5         = r29;
        const auto owner = runWorkerOwner(guest, L0, Opts{}, model);
        const auto c     = take(kW);
        UNSCOPED_INFO("worker: the bypass still runs the retained retrain");
        CHECK((owner.path == "bypass" && owner.importCalls == 0 && owner.retrainCalls == 1));
        UNSCOPED_INFO("worker: a zero low word of r29 counts no selected work");
        CHECK((c.counts.enter == 0 && c.counts.pending == 0 && c.quality() == 0 && c.counts.exitOk == 0 && c.refusals() == 0));
        UNSCOPED_INFO("worker: the bypass reads, probes and logs nothing");
        CHECK((guest.environments == 0 && guest.reads == 0 && guest.logLines().empty()));
    }
    // Worker: the bypass nested under a live record does not disturb it.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L2);
        guest.install();
        Cvars cvars(kW, false, true);
        auto  outer = makeEntry(kW, L0);
        auto  copy  = outer;
        (void)hookReplace(kW, copy);
        WorkerModel model;
        model.r5 = 0;
        (void)runWorkerOwner(guest, L2, Opts{}, model);
        finishCall(guest, kW, outer);
        const auto c = take(kW);
        UNSCOPED_INFO("worker: a bypassed owner is invisible to a live pair");
        CHECK((c.counts.exitOk == 1 && c.quality() == 0 && c.counts.pending == 0));
    }
    // A selected call reaching the observers with no entry is a missing entry. The HSIO call is counted once at the
    // return; the worker is counted again at its post observer, whose nonzero r29 says a record was expected.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, false, true);
        finishCall(guest, info.id, makeEntry(info.id, L0));
        const auto          c        = take(info.id);
        const std::uint64_t expected = info.id == VideoImportId::EdramWorker ? 2 : 1;
        UNSCOPED_INFO(std::string(info.name) + ": a selected call with no entry is a missing entry, counted at each observer that expected a record");
        CHECK((c.counts.correlationLost == expected && c.counts.exitUnpaired == expected && c.counts.exitOk == 0 && c.counts.pending == 0));
    }
}

// Nonzero-r29 duplicate posts are unpaired; call-free HSIO posts stay silent.
void testDuplicatePost()
{
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, false, true);
        auto  entry = makeEntry(info.id, L0);
        (void)runOwner(info.id, guest, L0);
        const auto first = take(info.id);
        UNSCOPED_INFO(std::string(info.name) + ": the first call completes ok");
        CHECK((first.counts.exitOk == 1 && first.quality() == 0 && first.counts.pending == 0));
        auto ctx = atPost(info.id, entry);
        hookPost(info.id, ctx);
        const auto second = take(info.id);
        if (info.id == kW)
        {
            UNSCOPED_INFO("worker: a duplicate post is counted as an unpaired missing record");
            CHECK((second.counts.correlationLost == 1 && second.counts.exitUnpaired == 1 && second.counts.exitOk == 0 && second.counts.pending == 0));
            const auto last = ReportVideoImportStubs(true);
            UNSCOPED_INFO("worker: and the duplicate post keeps the final incomplete");
            CHECK(last.find("video_edram_worker_final_incomplete=1") != std::string::npos);
        }
        else
        {
            UNSCOPED_INFO("hsio: a post hook with no record stays silent");
            CHECK((second.counts.correlationLost == 0 && second.counts.exitUnpaired == 0 && second.counts.exitOk == 0 && second.quality() == 0));
        }
    }
}

void testImportIndependence()
{
    // Each import's pair on its own: the other import's hooks do nothing.
    for (const auto& info : kInfo)
    {
        const auto& other = infoOf(info.id == kH ? kW : kH);
        Guest       guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, true, true);
        auto  ctx = makeEntry(other.id, L0);
        auto  now = ctx;
        UNSCOPED_INFO(std::string(info.name) + ": the other import's replacement does nothing");
        CHECK((!hookReplace(other.id, ctx) && sameContext(ctx, now)));
        finishCall(guest, other.id, now);
        UNSCOPED_INFO(std::string(info.name) + ": and reads and logs nothing");
        CHECK((guest.reads == 0 && guest.environments == 0 && guest.logLines().empty()));
        const auto o = take(other.id);
        UNSCOPED_INFO(std::string(info.name) + ": the other import's counters stay at zero");
        CHECK((o.counts.enter == 0 && o.quality() == 0 && o.counts.pending == 0));
        const auto report = ReportVideoImportStubs();
        UNSCOPED_INFO(std::string(info.name) + ": only the enabled import is reported");
        CHECK((report.find(other.prefix) == std::string::npos && report.find(info.prefix) != std::string::npos));
    }
    // The stub on one import, the exit check on the other: separate decisions and counters.
    for (const bool hsioStub : { true, false })
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(hsioStub ? Four{ true, false, false, true } : Four{ false, true, true, false }, true);
        auto       h  = makeEntry(kH, L0);
        auto       w  = makeEntry(kW, L0);
        auto       hc = h, wc = w;
        const bool hr = hookReplace(kH, hc), wr = hookReplace(kW, wc);
        UNSCOPED_INFO("the stub import is consumed and the check import runs once");
        CHECK((hr == hsioStub && wr == !hsioStub));
        finishCall(guest, kH, h);
        finishCall(guest, kW, w);
        const auto hs = take(kH), ws = take(kW);
        UNSCOPED_INFO("each import counts its own call");
        CHECK((hs.counts.enter == 1 && ws.counts.enter == 1 && hs.counts.consumed == (hsioStub ? 1U : 0U) && ws.counts.consumed == (hsioStub ? 0U : 1U)));
        UNSCOPED_INFO("each import pairs its own record");
        CHECK((hs.counts.exitOk == 1 && ws.counts.exitOk == 1 && hs.quality() == 0 && ws.quality() == 0 && hs.counts.pending == 0 && ws.counts.pending == 0));
    }
    // Interleaved boundaries of the two imports on one thread and one frame.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(Four{ true, false, true, false }, true);
        auto  h  = makeEntry(kH, L0);
        auto  w  = makeEntry(kW, L0);
        auto  hc = h, wc = w;
        UNSCOPED_INFO("both consumed");
        CHECK((hookReplace(kH, hc) && hookReplace(kW, wc)));
        owedStores(guest, kH, L0);
        owedStores(guest, kW, L0);
        auto hr = atReturn(kH, h);
        auto wr = atReturn(kW, w);
        hookReturn(kH, hr);
        hookReturn(kW, wr);
        auto he = atEffect(kH, h);
        auto we = atEffect(kW, w);
        hookEffect(kW, we);
        hookEffect(kH, he);
        auto hp = atPost(kH, h);
        auto wp = atPost(kW, w);
        hookPost(kH, hp);
        hookPost(kW, wp);
        const auto hs = take(kH), ws = take(kW);
        UNSCOPED_INFO("interleaved imports never consume each other's record");
        CHECK((hs.counts.exitOk == 1 && ws.counts.exitOk == 1 && hs.quality() == 0 && ws.quality() == 0));
    }
    // The pending budgets are separate.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(Four{ true, false, true, false }, true);
        SetVideoImportPendingLimitForTesting(kH, 0);
        auto h  = makeEntry(kH, L0);
        auto w  = makeEntry(kW, L0);
        auto hc = h, wc = w;
        UNSCOPED_INFO("an exhausted HSIO budget does not refuse the worker");
        CHECK((!hookReplace(kH, hc) && hookReplace(kW, wc)));
        finishCall(guest, kH, h);
        finishCall(guest, kW, w);
        const auto hs = take(kH), ws = take(kW);
        UNSCOPED_INFO("the refusal is counted by the import that owns the budget");
        CHECK((hs.refusal(R::Unpaired) == 1 && ws.refusals() == 0 && ws.counts.exitOk == 1));
    }
    // The drains: each import reports only its own fields.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(Four{ true, false, true, false }, true);
        (void)runOwner(kH, guest, L0);
        (void)runOwner(kW, guest, L0);
        const auto hsio = ReportVideoHsioStub();
        UNSCOPED_INFO("the HSIO drain carries only HSIO fields");
        CHECK((hsio.find(" video_hsio_enter=1 ") == 0 && hsio.find("video_edram_worker_") == std::string::npos));
        const auto worker = ReportVideoEdramWorkerStub();
        UNSCOPED_INFO("the worker drain carries only worker fields");
        CHECK((worker.find(" video_edram_worker_enter=1 ") == 0 && worker.find("video_hsio_") == std::string::npos));
        (void)runOwner(kH, guest, L0);
        (void)runOwner(kW, guest, L0);
        const auto both = ReportVideoImportStubs();
        UNSCOPED_INFO("the combined drain carries both, HSIO first");
        CHECK((both.find(" video_hsio_enter=1 ") == 0 && both.find(" video_edram_worker_enter=1 ") != std::string::npos));
    }
}

// ---------------------------------------------------------------------------
// The owner harnesses: the SDK run and the stub run of the generated owners.

struct Side
{
    Owner                                 owner;
    std::map<std::uint32_t, std::uint8_t> bytes;
    Counts                                counts;
    std::vector<std::string>              logs;
};

Side runSide(Id id, bool stub, bool check, bool hooks, const HsioModel& hsio = {}, const WorkerModel& worker = {}, const Layout& l = L0)
{
    Guest guest;
    setup(guest, l);
    guest.install();
    Cvars cvars(id, stub, check);
    Side  side;
    Opts  opts;
    opts.hooks = hooks;
    // The cached-bypass fixtures of HSIO load a nonzero word before the owner runs.
    side.owner  = id == kH ? runHsioOwner(guest, l, opts, hsio) : runWorkerOwner(guest, l, opts, worker);
    side.counts = take(id);
    side.bytes  = guest.snapshot();
    side.logs   = guest.logLines();
    return side;
}

// The stub run against the SDK run: equal except the LR that the skipped bl would have set.
void compareRuns(const std::string& name, const Info& info, const Side& sdk, const Side& stub)
{
    UNSCOPED_INFO(name + ": the same owner path");
    CHECK((sdk.owner.path == stub.owner.path && sdk.owner.path != ""));
    UNSCOPED_INFO(name + ": the entry context is the same");
    CHECK((sdk.owner.atHook.lr == info.entryLr && stub.owner.atHook.lr == info.entryLr && sameContext(sdk.owner.atHook, stub.owner.atHook)));
    UNSCOPED_INFO(name + ": at the return the SDK run has LR " + std::format("{:08X}", info.truePC) + " and the stub run keeps the entry LR");
    CHECK((sdk.owner.atReturn.lr == info.truePC && stub.owner.atReturn.lr == info.entryLr));
    UNSCOPED_INFO(name + ": the context at the return is equal except LR");
    CHECK(sameExceptLr(sdk.owner.atReturn, stub.owner.atReturn));
    UNSCOPED_INFO(name + ": the guest memory at the return is equal");
    CHECK(sdk.owner.memAtReturn == stub.owner.memAtReturn);
    UNSCOPED_INFO(name + ": the context at the effect boundary is equal except LR");
    CHECK(sameExceptLr(sdk.owner.atEffect, stub.owner.atEffect));
    UNSCOPED_INFO(name + ": after the next retained bl assigns LR the whole context is equal");
    CHECK(sameContext(sdk.owner.atChild, stub.owner.atChild));
    UNSCOPED_INFO(name + ": and the guest memory is equal");
    CHECK(sdk.owner.memAtChild == stub.owner.memAtChild);
    UNSCOPED_INFO(name + ": the post-child context is equal");
    CHECK(sameContext(sdk.owner.atPost, stub.owner.atPost));
    UNSCOPED_INFO(name + ": the final context is equal");
    CHECK(sameContext(sdk.owner.final, stub.owner.final));
    UNSCOPED_INFO(name + ": the whole guest memory is equal");
    CHECK(sdk.bytes == stub.bytes);
    UNSCOPED_INFO(name + ": the store trace is equal");
    CHECK(sdk.owner.stores == stub.owner.stores);
    UNSCOPED_INFO(name + ": the retained calls run the same number of times");
    CHECK((sdk.owner.childCalls == stub.owner.childCalls && sdk.owner.diagCalls == stub.owner.diagCalls && sdk.owner.retrainCalls == stub.owner.retrainCalls && sdk.owner.c7a8Calls == stub.owner.c7a8Calls &&
           sdk.owner.c810Calls == stub.owner.c810Calls));
}

void testHsioOwner()
{
    const auto& info = infoOf(kH);
    const auto  sdk  = runSide(kH, false, false, false);
    const auto  stub = runSide(kH, true, false, true);
    UNSCOPED_INFO("hsio: the SDK run calls the import once");
    CHECK((sdk.owner.path == "selected" && sdk.owner.importCalls == 1 && !sdk.owner.replaced));
    UNSCOPED_INFO("hsio: the stub run calls it zero times");
    CHECK((stub.owner.importCalls == 0 && stub.owner.replaced && stub.counts.counts.consumed == 1 && stub.counts.counts.original == 0));
    compareRuns("hsio", info, sdk, stub);
    // The preserved effects, asserted on their own: cache, branch, CR0 and the retained child.
    UNSCOPED_INFO("hsio: the retained child runs once in both runs");
    CHECK((stub.owner.trace == std::vector<std::string>{ "child" } && sdk.owner.trace == (std::vector<std::string>{ "import", "child" })));
    UNSCOPED_INFO("hsio: the owner stores zero at 0x82F9B0A4 exactly once");
    CHECK((stub.owner.stores.size() == 2 && stub.owner.stores[0] == std::make_pair(kCache, std::uint32_t{ 0 }) && stub.owner.stores[0] == sdk.owner.stores[0]));
    UNSCOPED_INFO("hsio: no diagnostic after result 1");
    CHECK((stub.owner.diagCalls == 0 && sdk.owner.diagCalls == 0));
    UNSCOPED_INFO("hsio: CR0 is equal only and r11 is zero at the effect");
    CHECK((stub.owner.atEffect.cr0.eq == 1 && stub.owner.atEffect.cr0.lt == 0 && stub.owner.atEffect.cr0.gt == 0 && stub.owner.atEffect.r11.u32 == 0));
    UNSCOPED_INFO("hsio: the cache word is zero at the effect");
    CHECK((stub.owner.cacheAtEffect == 0 && sdk.owner.cacheAtEffect == 0));
    UNSCOPED_INFO("hsio: the full r3 is 1 at the return in both runs");
    CHECK((stub.owner.atReturn.r3.u64 == 1 && sdk.owner.atReturn.r3.u64 == 1));
    UNSCOPED_INFO("hsio: the stub run's pair is ok");
    CHECK((stub.counts.counts.exitOk == 1 && stub.counts.quality() == 0 && stub.counts.counts.pending == 0));
    UNSCOPED_INFO("hsio: the record is the replaced call's");
    CHECK((stub.logs.size() == 1 && stub.logs[0].find(" replaced=1 ") != std::string::npos && stub.logs[0].find(" post_lr=00000000826A65CC ") != std::string::npos));

    // The summary-overflow bit is retained by the owner's compare and accepted.
    {
        HsioModel model;
        model.so          = 1;
        const auto sdkSo  = runSide(kH, false, false, false, model);
        const auto stubSo = runSide(kH, true, false, true, model);
        compareRuns("hsio so", info, sdkSo, stubSo);
        UNSCOPED_INFO("hsio: the retained summary overflow does not fail the pair");
        CHECK((stubSo.owner.atEffect.cr0.so == 1 && stubSo.owner.atEffect.cr0.eq == 1 && stubSo.counts.counts.exitOk == 1));
    }
    // The exit check on its own runs the import and pairs ok.
    {
        const auto check = runSide(kH, false, true, true);
        UNSCOPED_INFO("hsio: the exit check run equals the plain SDK run");
        CHECK((sameContext(check.owner.atReturn, sdk.owner.atReturn) && sameContext(check.owner.atChild, sdk.owner.atChild) && sameContext(check.owner.final, sdk.owner.final) && check.bytes == sdk.bytes));
        UNSCOPED_INFO("hsio: the exit check runs the import and pairs ok");
        CHECK((check.owner.importCalls == 1 && check.counts.counts.exitOk == 1 && check.counts.counts.consumed == 0));
    }
    // The cached nonzero word: no call, no selected work, everything equal to a plain run.
    {
        HsioModel model;
        model.cache       = 1;
        const auto plain  = runSide(kH, false, false, false, model);
        const auto hooked = runSide(kH, true, true, true, model);
        UNSCOPED_INFO("hsio: the cached path runs its diagnostic once");
        CHECK((hooked.owner.path == "cached" && hooked.owner.importCalls == 0 && hooked.owner.diagCalls == plain.owner.diagCalls && plain.owner.diagCalls == 1));
        UNSCOPED_INFO("hsio: the hooks change nothing on the cached path");
        CHECK((sameContext(plain.owner.final, hooked.owner.final) && plain.bytes == hooked.bytes && plain.owner.stores == hooked.owner.stores));
        UNSCOPED_INFO("hsio: the cached path counts nothing");
        CHECK((hooked.counts.counts.enter == 0 && hooked.counts.quality() == 0 && hooked.counts.counts.pending == 0));
    }
    // A zero import result (a fixture the SDK never returns): the exit check sees a result and effect mismatch,
    // and a stub that returned 0 would not match the retained diagnostics or the cache store.
    {
        HsioModel model;
        model.importResult = 0;
        const auto zero    = runSide(kH, false, true, true, model);
        UNSCOPED_INFO("hsio: result 0 stores 1 in the cache and runs the diagnostics");
        CHECK((zero.owner.diagCalls == 1 && zero.owner.stores[0] == std::make_pair(kCache, std::uint32_t{ 1 })));
        UNSCOPED_INFO("hsio: the exit check reports the result and effect mismatch");
        CHECK((zero.counts.mismatch(M::Return) == 1 && zero.counts.mismatch(M::Effect) == 1 && zero.counts.counts.exitOk == 0 && zero.counts.mismatches() == 2));
    }
}

void testWorkerOwner()
{
    const auto& info = infoOf(kW);
    const auto  sdk  = runSide(kW, false, false, false);
    const auto  stub = runSide(kW, true, false, true);
    UNSCOPED_INFO("worker: the SDK run calls the import once");
    CHECK((sdk.owner.path == "selected" && sdk.owner.importCalls == 1 && !sdk.owner.replaced));
    UNSCOPED_INFO("worker: the stub run calls it zero times");
    CHECK((stub.owner.importCalls == 0 && stub.owner.replaced && stub.counts.counts.consumed == 1 && stub.counts.counts.original == 0));
    compareRuns("worker", info, sdk, stub);
    const std::vector<std::string> sdkTrace{ "KeEnterCriticalRegion", "RtlEnterCriticalSection", "import", "retrain", "RtlLeaveCriticalSection", "KeLeaveCriticalRegion" };
    auto                           stubTrace = sdkTrace;
    stubTrace.erase(stubTrace.begin() + 2);
    UNSCOPED_INFO("worker: the critical region and section order is kept around one retained retrain");
    CHECK((sdk.owner.trace == sdkTrace && stub.owner.trace == stubTrace));
    UNSCOPED_INFO("worker: the retained retrain runs once on return 0");
    CHECK((stub.owner.retrainCalls == 1 && stub.owner.c7a8Calls == 0 && stub.owner.c810Calls == 0));
    UNSCOPED_INFO("worker: both stack words are written in the owner's order");
    CHECK((stub.owner.stores.size() == 2 && stub.owner.stores[0] == std::make_pair(L0.sp + 84, std::uint32_t{ 0 }) && stub.owner.stores[1] == std::make_pair(L0.sp + 80, std::uint32_t{ 0 })));
    UNSCOPED_INFO("worker: the retrain arguments are the owner's");
    CHECK((stub.owner.atEffect.r4.u64 == L0.sp + 80 && stub.owner.atEffect.r5.u64 == 4096 && stub.owner.atEffect.r6.u64 == L0.sp + 84 && stub.owner.atEffect.r7.u64 == L0.sp + 96 && stub.owner.atEffect.r8.u64 == 2048 && stub.owner.atEffect.r3.u64 == kWorkerR30));
    UNSCOPED_INFO("worker: the full r3 is 0 at the return and the owner returns 1");
    CHECK((stub.owner.atReturn.r3.u64 == 0 && sdk.owner.atReturn.r3.u64 == 0 && stub.owner.final.r3.u64 == 1));
    UNSCOPED_INFO("worker: the stub run's pair is ok");
    CHECK((stub.counts.counts.exitOk == 1 && stub.counts.quality() == 0 && stub.counts.counts.pending == 0));
    UNSCOPED_INFO("worker: the record is the replaced call's");
    CHECK((stub.logs.size() == 1 && stub.logs[0].find(" post_lr=00000000826B0568 ") != std::string::npos && stub.logs[0].find(" replaced=1 ") != std::string::npos));

    // A modeled nonzero retrain result exercises the retained retry path in both runs.
    {
        WorkerModel model;
        model.retrain        = { 5, 0 };
        const auto sdkRetry  = runSide(kW, false, false, false, {}, model);
        const auto stubRetry = runSide(kW, true, false, true, {}, model);
        compareRuns("worker retry", info, sdkRetry, stubRetry);
        UNSCOPED_INFO("worker: the retry loop runs its second retrain and cursor update");
        CHECK((stubRetry.owner.retrainCalls == 2 && stubRetry.owner.c7a8Calls == 1 && stubRetry.owner.c810Calls == 1));
        UNSCOPED_INFO("worker: a nonzero retrain result at 0x826B0568 is still ok");
        CHECK((stubRetry.counts.counts.exitOk == 1 && stubRetry.counts.quality() == 0));
        UNSCOPED_INFO("worker: the record keeps the retrain's r3");
        CHECK((stubRetry.logs.size() == 1 && stubRetry.logs[0].find(" post_r3=0000000000000005 ") != std::string::npos));
    }
    // A second modeled retry with the owner's r29 low word zero after the first retry iteration.
    {
        WorkerModel model;
        model.retrain        = { 7, 3, 0 };
        const auto sdkRetry  = runSide(kW, false, false, false, {}, model);
        const auto stubRetry = runSide(kW, true, false, true, {}, model);
        compareRuns("worker second retry", info, sdkRetry, stubRetry);
        UNSCOPED_INFO("worker: the loop runs while the retained result is nonzero");
        CHECK((stubRetry.owner.retrainCalls == 3 && stubRetry.owner.c7a8Calls == 2 && stubRetry.owner.c810Calls == 2));
    }
    // The exit check on its own runs the import and pairs ok.
    {
        const auto check = runSide(kW, false, true, true);
        UNSCOPED_INFO("worker: the exit check runs the import and pairs ok");
        CHECK((check.owner.importCalls == 1 && check.counts.counts.exitOk == 1 && check.counts.counts.consumed == 0 && sameContext(check.owner.final, sdk.owner.final)));
    }
    // The bypass runs the retained retrain with every worker observer silent.
    {
        WorkerModel model;
        model.r5          = 0;
        const auto plain  = runSide(kW, false, false, false, {}, model);
        const auto hooked = runSide(kW, true, true, true, {}, model);
        UNSCOPED_INFO("worker: the bypass runs the retained retrain once");
        CHECK((hooked.owner.path == "bypass" && hooked.owner.importCalls == 0 && hooked.owner.retrainCalls == 1 && plain.owner.retrainCalls == 1));
        UNSCOPED_INFO("worker: the hooks change nothing on the bypass");
        CHECK((sameContext(plain.owner.final, hooked.owner.final) && plain.bytes == hooked.bytes && plain.owner.stores == hooked.owner.stores));
        UNSCOPED_INFO("worker: the bypass counts nothing");
        CHECK((hooked.counts.counts.enter == 0 && hooked.counts.quality() == 0 && hooked.counts.counts.pending == 0));
    }
    // Selected with a high-only r29: the owner's low word compare takes the bypass.
    {
        WorkerModel model;
        model.r5          = 0xFFFFFFFF00000000ULL;
        const auto hooked = runSide(kW, true, true, true, {}, model);
        UNSCOPED_INFO("worker: a high-only r29 is the bypass");
        CHECK((hooked.owner.path == "bypass" && hooked.counts.counts.enter == 0));
    }
}

void testHookAbi()
{
    static_assert(std::is_same_v<decltype(&ReplaceNativeVideoHsioCall), bool (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoHsioReturn), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoHsioEffect), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCCRRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoHsioPostChild), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCCRRegister&, uint64_t)>);
    static_assert(std::is_same_v<decltype(&ReplaceNativeVideoEdramWorkerCall), bool (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoEdramWorkerReturn), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoEdramWorkerPreRetrain),
                                 void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoEdramWorkerPostRetrain),
                                 void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)>);

    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        Guest             guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(info.id, true, false);
        const auto entry    = makeEntry(info.id, L0);
        auto       ctx      = entry;
        auto       expected = entry;
        UNSCOPED_INFO(name + ": the hook replaces the call");
        CHECK(hookReplace(info.id, ctx));
        expected.r3.u64 = info.fullR3;
        UNSCOPED_INFO(name + ": only r3 changed, whatever the other register classes held");
        CHECK((ctx.r3.u64 == info.fullR3 && sameContext(ctx, expected)));
        // The observers change no register, including CR0 and LR.
        auto ret  = atReturn(info.id, entry);
        auto copy = ret;
        hookReturn(info.id, ret);
        UNSCOPED_INFO(name + ": the return observer changes no register");
        CHECK(sameContext(ret, copy));
        owedStores(guest, info.id, L0);
        auto eff = atEffect(info.id, entry);
        copy     = eff;
        hookEffect(info.id, eff);
        UNSCOPED_INFO(name + ": the effect observer changes no register");
        CHECK(sameContext(eff, copy));
        auto post = atPost(info.id, entry);
        copy      = post;
        hookPost(info.id, post);
        UNSCOPED_INFO(name + ": the post-child observer changes no register");
        CHECK(sameContext(post, copy));
        const auto counts = take(info.id);
        UNSCOPED_INFO(name + ": one consumed, ok, none outstanding");
        CHECK((counts.counts.consumed == 1 && counts.counts.exitOk == 1 && counts.counts.pending == 0));
        // A wide r3 or r30 is refused, never masked.
        auto wide = makeEntry(info.id, L0);
        if (info.id == kH)
            wide.r30.u64 = 0x0000000082FA0000ULL;
        else
            wide.r3.u64 = 0x0000000100000000ULL;
        const auto wideCopy = wide;
        UNSCOPED_INFO(name + ": a wide register is not the owner's call");
        CHECK((!hookReplace(info.id, wide) && sameContext(wide, wideCopy)));
        takeAll();
        finishCall(guest, info.id, wideCopy);
        takeAll();
        // A wide r31 is a different device than the validated one.
        auto wideDevice = makeEntry(info.id, L0);
        wideDevice.r31.u64 |= 0x1111000000000000ULL;
        const auto deviceCopy = wideDevice;
        UNSCOPED_INFO(name + ": the 32-bit device decides readiness");
        CHECK(hookReplace(info.id, wideDevice));
        auto expectedDevice   = deviceCopy;
        expectedDevice.r3.u64 = info.fullR3;
        UNSCOPED_INFO(name + ": and only r3 changes");
        CHECK(sameContext(wideDevice, expectedDevice));
        takeAll();
        finishCall(guest, info.id, deviceCopy);
        takeAll();
    }
}

void testReport()
{
    // The exact field names and order of one import.
    const auto expectedFields = [](const std::string& prefix, const VideoImportStubCounts& c, bool final)
    {
        const std::pair<const char*, std::uint64_t> fields[] = {
            { "enter", c.enter },
            { "ready_at_entry", c.readyAtEntry },
            { "consumed", c.consumed },
            { "original", c.original },
            { "refusal_pc", c.refusal[0] },
            { "refusal_services", c.refusal[1] },
            { "refusal_provider", c.refusal[2] },
            { "refusal_diagnostics", c.refusal[3] },
            { "refusal_stack", c.refusal[4] },
            { "refusal_shape", c.refusal[5] },
            { "refusal_unreadable", c.refusal[6] },
            { "refusal_reentrant", c.refusal[7] },
            { "refusal_unpaired", c.refusal[8] },
            { "exit_ok", c.exitOk },
            { "exit_mismatch_return", c.mismatch[0] },
            { "exit_mismatch_effect", c.mismatch[1] },
            { "exit_mismatch_continuation", c.mismatch[2] },
            { "exit_mismatch_lr", c.mismatch[3] },
            { "exit_unreadable", c.exitUnreadable },
            { "exit_unpaired", c.exitUnpaired },
            { "reentry", c.reentry },
            { "abandoned", c.abandoned },
            { "correlation_lost", c.correlationLost },
            { "reporting_failed", c.reportingFailed },
            { "pending", c.pending },
        };
        std::string text;
        for (const auto& [name, value] : fields)
            text += " " + prefix + name + "=" + std::to_string(value);
        if (final)
        {
            const bool incomplete = c.pending != 0 || c.finalSkipped != 0 || c.reportingFailed != 0 || c.unresolved;
            text += " " + prefix + "final_incomplete=" + (incomplete ? "1" : "0") + " " + prefix + "final_skipped=" + std::to_string(c.finalSkipped);
        }
        return text;
    };
    for (const auto& info : kInfo)
    {
        VideoImportStubCounts counts;
        std::uint64_t         next = 1;
        counts.enter               = next++;
        counts.readyAtEntry        = next++;
        counts.consumed            = next++;
        counts.original            = next++;
        for (auto& v : counts.refusal)
            v = next++;
        counts.exitOk = next++;
        for (auto& v : counts.mismatch)
            v = next++;
        counts.exitUnreadable  = next++;
        counts.exitUnpaired    = next++;
        counts.reentry         = next++;
        counts.abandoned       = next++;
        counts.correlationLost = next++;
        counts.reportingFailed = next++;
        counts.pending         = next++;
        const auto format      = [&](const VideoImportStubCounts& c)
        {
            return info.id == kH ? FormatVideoHsioStubCounts(c) : FormatVideoEdramWorkerStubCounts(c);
        };
        UNSCOPED_INFO(std::string(info.name) + ": the periodic fields, names and order");
        CHECK(format(counts) == expectedFields(info.prefix, counts, false));
        UNSCOPED_INFO(std::string(info.name) + ": a periodic line has no final markers");
        CHECK(format(counts).find("final") == std::string::npos);
        counts.final        = true;
        counts.finalSkipped = 3;
        UNSCOPED_INFO(std::string(info.name) + ": the final fields");
        CHECK(format(counts) == expectedFields(info.prefix, counts, true));
        VideoImportStubCounts clean;
        clean.final = true;
        UNSCOPED_INFO(std::string(info.name) + ": a clean final");
        CHECK(format(clean).find(std::string(" ") + info.prefix + "final_incomplete=0 " + info.prefix + "final_skipped=0") != std::string::npos);
        clean.pending = 1;
        UNSCOPED_INFO(std::string(info.name) + ": an outstanding call makes the final incomplete");
        CHECK(format(clean).find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
        clean            = {};
        clean.final      = true;
        clean.unresolved = true;
        UNSCOPED_INFO(std::string(info.name) + ": an earlier unresolved failure makes the final incomplete");
        CHECK(format(clean).find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
        clean              = {};
        clean.final        = true;
        clean.finalSkipped = 1;
        UNSCOPED_INFO(std::string(info.name) + ": a retained skipped final makes the final incomplete");
        CHECK(format(clean).find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
        clean                 = {};
        clean.final           = true;
        clean.reportingFailed = 1;
        UNSCOPED_INFO(std::string(info.name) + ": a reporting failure makes the final incomplete");
        CHECK(format(clean).find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
    }

    static std::vector<std::string> logs;
    logs.clear();
    const VideoImportReportOperations report{ [](VideoImportId which, const VideoImportStubCounts& c)
                                              {
                                                  return which == VideoImportId::Hsio ? FormatVideoHsioStubCounts(c) : FormatVideoEdramWorkerStubCounts(c);
                                              },
                                              [](std::string_view message)
                                              {
                                                  logs.emplace_back(message);
                                              } };
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(Four{ true, false, true, false }, true);
        // The timing windows sum, and the final reports the residual.
        std::uint64_t windowH = 0, windowW = 0;
        for (int i = 0; i < 3; ++i)
        {
            (void)runOwner(kH, guest, L0);
            (void)runOwner(kW, guest, L0);
        }
        auto window = ReportVideoImportStubs(false, report);
        UNSCOPED_INFO("the window reports both imports");
        CHECK((window.find(" video_hsio_enter=3 video_hsio_ready_at_entry=3 video_hsio_consumed=3") == 0 && window.find(" video_edram_worker_enter=3 ") != std::string::npos));
        windowH += 3;
        windowW += 3;
        for (int i = 0; i < 2; ++i)
        {
            (void)runOwner(kH, guest, L0);
            (void)runOwner(kW, guest, L0);
        }
        window = ReportVideoImportStubs(false, report);
        UNSCOPED_INFO("the next window starts at zero");
        CHECK(window.find(" video_hsio_enter=2 ") == 0);
        windowH += 2;
        windowW += 2;
        (void)runOwner(kH, guest, L0);
        (void)runOwner(kW, guest, L0);
        UNSCOPED_INFO("a periodic drain does not log");
        CHECK(logs.empty());
        const auto final = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO("the final reports the residual of both imports");
        CHECK((final.find(" video_hsio_enter=1 ") == 0 && final.find(" video_edram_worker_enter=1 ") != std::string::npos));
        UNSCOPED_INFO("the windows and the residual sum to the calls");
        CHECK((windowH + 1 == 6 && windowW + 1 == 6));
        UNSCOPED_INFO("each import's final is logged once, clean");
        CHECK((logs.size() == 2 && logs[0].find("video_hsio_final_incomplete=0") != std::string::npos && logs[1].find("video_edram_worker_final_incomplete=0") != std::string::npos));
        UNSCOPED_INFO("nothing was skipped");
        CHECK(final.find("final_skipped=0") != std::string::npos);
    }
    // One import's reporting intent does not report the other.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(kH, true, false);
        (void)runOwner(kH, guest, L0);
        logs.clear();
        const auto only = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO("only the requested import is reported and logged");
        CHECK((only.find("video_edram_worker_") == std::string::npos && logs.size() == 1));
        UNSCOPED_INFO("the single-import drains follow their own options");
        CHECK((ReportVideoEdramWorkerStub(true, report).empty() && ReportVideoHsioStub(false, report).find(" video_hsio_enter=0 ") == 0));
    }
    // A concurrent drain holding the lock does not stall shutdown, and each skip is retained.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(Four{ true, false, true, false }, true);
        logs.clear();
        auto& mutex = detail::VideoImportReportMutex();
        mutex.lock();
        const auto  begun = std::chrono::steady_clock::now();
        std::string skipped;
        std::thread t([&]
                      {
                          skipped = ReportVideoImportStubs(true, report);
                      });
        t.join();
        const auto elapsed = std::chrono::steady_clock::now() - begun;
        mutex.unlock();
        UNSCOPED_INFO("the final drain is bounded");
        CHECK(elapsed < std::chrono::seconds(2));
        UNSCOPED_INFO("each skipped final says so");
        CHECK((skipped.find("video_hsio_final_incomplete=1 video_hsio_final_skipped=1") != std::string::npos && skipped.find("video_edram_worker_final_incomplete=1 video_edram_worker_final_skipped=1") != std::string::npos));
        UNSCOPED_INFO("each skipped final is logged");
        CHECK((logs.size() == 2 && logs[0].find("video_hsio_final_skipped=1") != std::string::npos));
        const auto periodic = ReportVideoImportStubs(false, report);
        UNSCOPED_INFO("a timing drain does not consume the retained skip");
        CHECK(periodic.find("final") == std::string::npos);
        const auto next = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO("the skip is reported by the next final");
        CHECK((next.find("video_hsio_final_skipped=1") != std::string::npos && next.find("video_edram_worker_final_skipped=1") != std::string::npos));
        const auto last = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO("and only once");
        CHECK((last.find("video_hsio_final_skipped=0") != std::string::npos && last.find("video_edram_worker_final_skipped=0") != std::string::npos));
        UNSCOPED_INFO("the earlier failure keeps each final incomplete");
        CHECK((last.find("video_hsio_final_incomplete=1") != std::string::npos && last.find("video_edram_worker_final_incomplete=1") != std::string::npos));
    }
    // An outstanding call at shutdown is an incomplete final and the gauge survives it.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, true, false);
        auto  entry = makeEntry(info.id, L0);
        auto  ctx   = entry;
        UNSCOPED_INFO(std::string(info.name) + ": consumed");
        CHECK(hookReplace(info.id, ctx));
        const auto last = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO(std::string(info.name) + ": an unfinished record makes the final incomplete");
        CHECK((last.find(std::string(info.prefix) + "pending=1") != std::string::npos && last.find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos));
        UNSCOPED_INFO(std::string(info.name) + ": the gauge survives the final drain");
        CHECK(take(info.id).counts.pending == 1);
        finishCall(guest, info.id, entry);
        takeAll();
    }
    // A mismatch latches the unresolved flag: a later clean call and the timing drains cannot clear it.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        const Fault mismatch{ "return mismatch", info.id, [](Guest&, PPCContext& c)
                              {
                                  c.r3.u64 ^= 0x100000000ULL;
                              },
                              nullptr,
                              nullptr,
                              nullptr,
                              static_cast<int>(M::Return) };
        Cvars      cvars(info.id, false, true);
        const auto counts = runFaultConfigured(mismatch, guest);
        UNSCOPED_INFO(std::string(info.name) + ": the injected mismatch is counted");
        CHECK((counts.mismatch(M::Return) == 1 && counts.counts.exitOk == 0));
        (void)runOwner(info.id, guest, L0);
        UNSCOPED_INFO(std::string(info.name) + ": a later clean call is ok");
        CHECK(take(info.id).counts.exitOk == 1);
        const auto periodic = ReportVideoImportStubs(false, report);
        UNSCOPED_INFO(std::string(info.name) + ": a timing drain is not a final");
        CHECK(periodic.find(std::string(info.prefix) + "final_incomplete") == std::string::npos);
        const auto last = ReportVideoImportStubs(true, report);
        UNSCOPED_INFO(std::string(info.name) + ": a mismatch keeps the final incomplete across a clean call and a timing drain");
        CHECK(last.find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
        takeAll();
    }
    // A throwing format hides nothing: the failure is counted and the final stays incomplete.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars                             cvars(info.id, true, false);
        const VideoImportReportOperations throwingFormat{ [](VideoImportId, const VideoImportStubCounts&) -> std::string
                                                          {
                                                              throw std::runtime_error("injected format fault");
                                                          },
                                                          [](std::string_view)
                                                          {
                                                          } };
        const auto drain = [&](bool shutdown, const VideoImportReportOperations& operations)
        {
            return info.id == kH ? ReportVideoHsioStub(shutdown, operations) : ReportVideoEdramWorkerStub(shutdown, operations);
        };
        UNSCOPED_INFO(std::string(info.name) + ": a failed format returns no fields");
        CHECK(drain(false, throwingFormat).empty());
        const auto later = drain(false, report);
        UNSCOPED_INFO(std::string(info.name) + ": the failure is visible in the next window");
        CHECK(later.find(std::string(" ") + info.prefix + "reporting_failed=1 ") != std::string::npos);
        const auto final = drain(true, report);
        UNSCOPED_INFO(std::string(info.name) + ": and keeps the final incomplete after the error drain");
        CHECK(final.find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos);
    }
    // A throwing logger is contained, still returns the fields and is counted.
    for (const auto& info : kInfo)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars                             cvars(info.id, true, false);
        const VideoImportReportOperations throwing{ report.format, [](std::string_view)
                                                    {
                                                        throw std::runtime_error("injected report fault");
                                                    } };
        const auto drain = [&](const VideoImportReportOperations& operations)
        {
            return info.id == kH ? ReportVideoHsioStub(true, operations) : ReportVideoEdramWorkerStub(true, operations);
        };
        UNSCOPED_INFO(std::string(info.name) + ": a throwing logger still returns the fields");
        CHECK(!drain(throwing).empty());
        const auto next = drain(report);
        UNSCOPED_INFO(std::string(info.name) + ": the logger failure persists into the next final");
        CHECK((next.find(std::string(info.prefix) + "reporting_failed=1") != std::string::npos && next.find(std::string(info.prefix) + "final_incomplete=1") != std::string::npos));
    }
}

// The first sixteen ok lines are logged, every other record always, with every named field.
void testLogging()
{
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        Guest             guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(info.id, true, false);
        for (int i = 0; i < 20; ++i)
            (void)runOwner(info.id, guest, L0);
        auto logs = guest.logLines();
        UNSCOPED_INFO(name + ": only the first sixteen ok records are logged");
        CHECK(logs.size() == 16);
        UNSCOPED_INFO(name + ": all twenty pair ok");
        CHECK(take(info.id).counts.exitOk == 20);
        // A mismatch is always logged: the exit check with a wrong result.
        {
            auto entry = makeEntry(info.id, L0);
            auto copy  = entry;
            (void)hookReplace(info.id, copy);
            auto ret = atReturn(info.id, entry);
            ret.r3.u64 ^= 2;
            hookReturn(info.id, ret);
            owedStores(guest, info.id, L0);
            auto eff = atEffect(info.id, entry);
            hookEffect(info.id, eff);
            auto post = atPost(info.id, entry);
            hookPost(info.id, post);
        }
        logs = guest.logLines();
        UNSCOPED_INFO(name + ": a bad record is always logged");
        CHECK((logs.size() == 17 && logs.back().find("verdict=mismatch") != std::string::npos));
        (void)take(info.id);
        const char* common[] = { "import=", "call_pc=", "true_pc=", "pc=", "thread=", "host_thread=", "invocation=", "sp=", "sp_full=", "g=", "g_full=", "entry_r3=", "entry_r11=", "entry_r29=", "entry_r30=", "entry_lr=", "runtime_id=", "delegate_id=", "ring_service_id=", "service_generation=", "provider_target=", "provider_valid=1", "ready_at_entry=1", "replaced=1", "frame_valid=1", "result_valid=1", "result_full=", "return_lr_valid=0", "effect_valid=1", "post_valid=1", "post_r3=", "post_lr=", "identity_changed=0", "verdict=ok" };
        for (const auto* field : common)
        {
            UNSCOPED_INFO(name + ": the log carries " + field);
            CHECK(logs[0].find(std::string(" ") + field) != std::string::npos);
        }
        const char* hsio[]   = { "cache_entry=00000000", "cache_effect=00000000", "effect_r11=", "effect_cr0=00000002", "post_r11=", "post_cr0=00000008" };
        const char* worker[] = { "pre_r3=", "pre_r4=", "pre_r5=", "pre_r6=", "pre_r7=", "pre_r8=", "pre_stack80=", "pre_stack84=", "post_r4=", "post_r5=", "post_r6=", "post_r7=", "post_r8=", "post_stack80=", "post_stack84=" };
        if (info.id == kH)
            for (const auto* field : hsio)
            {
                UNSCOPED_INFO(name + ": the log carries " + field);
                CHECK(logs[0].find(std::string(" ") + field) != std::string::npos);
            }
        else
            for (const auto* field : worker)
            {
                UNSCOPED_INFO(name + ": the log carries " + field);
                CHECK(logs[0].find(std::string(" ") + field) != std::string::npos);
            }
        UNSCOPED_INFO(name + ": the row's PCs are recorded");
        CHECK((logs[0].find(std::format(" call_pc={:08X} ", info.callPC)) != std::string::npos && logs[0].find(std::format(" true_pc={:08X} ", info.truePC)) != std::string::npos && logs[0].find(std::format(" pc={:08X} ", info.postPC)) != std::string::npos));
        UNSCOPED_INFO(name + ": the raw entry LR, result and guest thread are recorded");
        CHECK((logs[0].find(std::format(" entry_lr={:016X} ", info.entryLr)) != std::string::npos && logs[0].find(std::format(" result_full={:016X} ", info.fullR3)) != std::string::npos && logs[0].find(std::format(" thread={:016X} ", kThread)) != std::string::npos));
        UNSCOPED_INFO(name + ": the stack pointer and device are recorded");
        CHECK(logs[0].find(std::format(" sp={:08X} sp_full={:016X} g={:08X} g_full={:016X} ", L0.sp, std::uint64_t{ L0.sp }, L0.g, std::uint64_t{ L0.g })) != std::string::npos);
        UNSCOPED_INFO(name + ": the post-child LR is recorded");
        CHECK(logs[0].find(std::format(" post_lr={:016X} ", info.postPC)) != std::string::npos);
        UNSCOPED_INFO(name + ": the line names the import");
        CHECK(logs[0].rfind(info.id == kH ? "Native video HSIO check:" : "Native video EDRAM worker check:", 0) == 0);
        // A throwing logger never changes the counts.
        {
            guest.throwOnLog = true;
            auto entry       = makeEntry(info.id, L0);
            auto copy        = entry;
            (void)hookReplace(info.id, copy);
            auto ret = atReturn(info.id, entry);
            ret.r3.u64 ^= 2;
            hookReturn(info.id, ret);
            owedStores(guest, info.id, L0);
            auto eff = atEffect(info.id, entry);
            hookEffect(info.id, eff);
            auto post = atPost(info.id, entry);
            hookPost(info.id, post);
            const auto counts = take(info.id);
            UNSCOPED_INFO(name + ": a throwing logger is contained and counted");
            CHECK((counts.counts.exitOk == 0 && counts.mismatch(M::Return) == 1 && counts.counts.pending == 0 && counts.counts.reportingFailed == 1));
            guest.throwOnLog = false;
        }
    }
}

// ---------------------------------------------------------------------------
// The real SDK exports over the runtime's guest memory.

struct RealMemory
{
    rex::memory::Memory* memory;

    std::uint32_t get32(std::uint32_t address) const
    {
        std::uint32_t raw = 0;
        std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), 4);
        return rex::byte_swap(raw);
    }

    void put32(std::uint32_t address, std::uint32_t value) const
    {
        const auto raw = rex::byte_swap(value);
        std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), &raw, 4);
    }

    std::vector<std::uint8_t> snapshot(std::uint32_t address, std::uint32_t size) const
    {
        std::vector<std::uint8_t> bytes(size);
        std::memcpy(bytes.data(), memory->TranslateVirtual<const std::uint8_t*>(address), size);
        return bytes;
    }
};

void testRuntimeParity()
{
    rex::Runtime runtime(std::filesystem::path{});
    UNSCOPED_INFO("runtime setup");
    CHECK(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == X_STATUS_SUCCESS);
    auto* memory = runtime.memory();
    UNSCOPED_INFO("guest memory");
    CHECK(memory != nullptr);
    if (!memory)
        return;
    const RealMemory real{ memory };
    auto* const      base = memory->virtual_membase();

    // The actual exports against pure Apply, with poisoned unrelated registers and high halves.
    {
        const std::uint32_t scratch = memory->SystemHeapAlloc(0x1000, 0x1000, rex::memory::kSystemHeapVirtual);
        UNSCOPED_INFO("scratch allocation");
        CHECK(scratch != 0);
        if (scratch)
        {
            for (std::uint32_t offset = 0; offset < 0x1000; offset += 4)
                real.put32(scratch + offset, 0x5A000000 | offset);
            const auto image = real.snapshot(scratch, 0x1000);
            for (const auto& info : kInfo)
            {
                const auto& row = rowOf(info.id);
                for (const std::uint64_t input : std::initializer_list<std::uint64_t>{ std::uint64_t{ 0 }, std::uint64_t{ 1 }, 0xFFFFFFFF00000000ULL, 0x00000001FFFFFFFFULL, ~0ULL, kStale })
                {
                    PPCContext sdk{};
                    std::memset(&sdk, 0x35, sizeof(sdk));
                    sdk.r3.u64        = input;
                    sdk.lr            = info.entryLr;
                    const auto before = sdk;
                    if (info.id == kH)
                        __imp__VdIsHSIOTrainingSucceeded(sdk, base);
                    else
                        __imp__VdRetrainEDRAMWorker(sdk, base);
                    auto expected   = before;
                    expected.r3.u64 = info.fullR3;
                    UNSCOPED_INFO(std::string(info.name) + ": the SDK export returns the row's constant in the full r3");
                    CHECK(sdk.r3.u64 == info.fullR3);
                    UNSCOPED_INFO(std::string(info.name) + ": the SDK export changes no other register");
                    CHECK(sameContext(sdk, expected));
                    UNSCOPED_INFO(std::string(info.name) + ": the SDK export writes no guest byte");
                    CHECK(real.snapshot(scratch, 0x1000) == image);
                    std::uint64_t applied = input;
                    ApplyVideoImportStub([&]
                                         {
                                             VideoImportStubPlan plan;
                                             plan.row    = &row;
                                             plan.fullR3 = row.fullR3;
                                             return plan;
                                         }(),
                                         applied);
                    UNSCOPED_INFO(std::string(info.name) + ": pure Apply equals the SDK export");
                    CHECK(applied == sdk.r3.u64);
                }
            }
        }
    }

    // The owners over the actual exports: the plain SDK run, the exit-check baseline and the stub candidate.
    HsioModel hsio;
    hsio.real = true;
    hsio.base = base;
    WorkerModel worker;
    worker.real = true;
    worker.base = base;
    for (const auto& info : kInfo)
    {
        const auto        plain     = runSide(info.id, false, false, false, hsio, worker);
        const auto        baseline  = runSide(info.id, false, true, true, hsio, worker);
        const auto        candidate = runSide(info.id, true, false, true, hsio, worker);
        const std::string name      = std::string(info.name) + " (actual exports)";
        UNSCOPED_INFO(name + ": the stub skips only the import");
        CHECK((plain.owner.importCalls == 1 && baseline.owner.importCalls == 1 && candidate.owner.importCalls == 0));
        UNSCOPED_INFO(name + ": the baseline pairs ok");
        CHECK((baseline.counts.counts.enter == 1 && baseline.counts.counts.original == 1 && baseline.counts.counts.exitOk == 1 && baseline.counts.refusals() == 0 && baseline.counts.mismatches() == 0));
        UNSCOPED_INFO(name + ": the candidate is consumed and pairs ok");
        CHECK((candidate.counts.counts.consumed == 1 && candidate.counts.counts.exitOk == 1 && candidate.counts.refusals() == 0 && candidate.counts.mismatches() == 0));
        compareRuns(name, info, plain, candidate);
        UNSCOPED_INFO(name + ": the baseline equals the plain run");
        CHECK((sameContext(plain.owner.final, baseline.owner.final) && plain.bytes == baseline.bytes));
        UNSCOPED_INFO(name + ": the actual export and the candidate both give the row's full r3");
        CHECK((plain.owner.atReturn.r3.u64 == info.fullR3 && candidate.owner.atReturn.r3.u64 == info.fullR3));
        UNSCOPED_INFO(name + ": one record each");
        CHECK((baseline.logs.size() == 1 && candidate.logs.size() == 1));
        const auto field = [](const std::string& line, const std::string& key)
        {
            const auto at = line.find(" " + key + "=");
            if (at == std::string::npos)
                return std::string{};
            const auto begin = at + key.size() + 2;
            return line.substr(begin, line.find(' ', begin) - begin);
        };
        UNSCOPED_INFO(name + ": the baseline's and the candidate's recorded results are equal");
        CHECK((!field(baseline.logs[0], "result_full").empty() && field(baseline.logs[0], "result_full") == field(candidate.logs[0], "result_full") && field(baseline.logs[0], "post_r3") == field(candidate.logs[0], "post_r3")));
    }
}

void WrongImportTarget(PPCContext&, uint8_t*)
{
}

// Tool-mode Runtime has guest memory without graphics; fakes cover admission.
void testRuntimeAdmission()
{
    rex::Runtime runtime(std::filesystem::path{});
    UNSCOPED_INFO("runtime setup");
    CHECK(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == X_STATUS_SUCCESS);
    auto* memory = runtime.memory();
    UNSCOPED_INFO("guest memory and no graphics system");
    CHECK((memory != nullptr && runtime.graphics_system() == nullptr));
    if (!memory)
        return;
    const RealMemory real{ memory };
    SetVideoImportOperationsForTesting(nullptr);
    const std::uint32_t frame  = memory->SystemHeapAlloc(0x3000, 0x1000, rex::memory::kSystemHeapVirtual);
    const std::uint32_t device = memory->SystemHeapAlloc(0x6000, 0x1000, rex::memory::kSystemHeapVirtual);
    UNSCOPED_INFO("allocations");
    CHECK((frame && device));
    if (!(frame && device))
        return;
    const Layout layout{ frame + 0x1000, device };
    for (std::uint32_t offset = 0; offset < 0x3000; offset += 4)
        real.put32(frame + offset, 0x5A000000 | offset);
    auto* dispatcher = runtime.function_dispatcher();
    UNSCOPED_INFO("dispatcher");
    CHECK(dispatcher != nullptr);
    if (!dispatcher)
        return;
    UNSCOPED_INFO("a function table over the title image");
    CHECK(dispatcher->InitializeFunctionTable(0x82000000, 0x00F00000, 0x82000000, 0x00F00000));
    for (const auto& info : kInfo)
    {
        const std::string name = info.name;
        const auto&       row  = rowOf(info.id);
        Cvars             cvars(info.id, true, false, true, layout.g);
        // The environment as the production probe reads it.
        auto environment = ProductionVideoImportEnvironmentForTesting(row);
        UNSCOPED_INFO(name + ": the Runtime is found");
        CHECK((environment.runtime && environment.identity.runtime != 0));
        UNSCOPED_INFO(name + ": no graphics system and no detached delegate");
        CHECK((!environment.graphics && !environment.delegate && environment.identity.delegate == 0 && environment.delegateOwner == nullptr));
        UNSCOPED_INFO(name + ": a different function at the import");
        CHECK(dispatcher->SetFunction(info.importAddress, &WrongImportTarget));
        environment = ProductionVideoImportEnvironmentForTesting(row);
        UNSCOPED_INFO(name + ": a different target is present");
        CHECK((environment.dispatcher && environment.target == &WrongImportTarget && environment.identity.target == reinterpret_cast<std::uintptr_t>(&WrongImportTarget)));
        UNSCOPED_INFO(name + ": and is not the row's wrapper");
        CHECK(environment.target != row.wrapper);
        UNSCOPED_INFO(name + ": the retained wrapper at the import");
        CHECK(dispatcher->SetFunction(info.importAddress, row.wrapper));
        environment = ProductionVideoImportEnvironmentForTesting(row);
        UNSCOPED_INFO(name + ": the retained wrapper matches");
        CHECK((environment.target == row.wrapper && environment.identity.target == reinterpret_cast<std::uintptr_t>(row.wrapper)));
        UNSCOPED_INFO(name + ": the ring services identity is bound");
        CHECK(environment.identity.ring == reinterpret_cast<std::uintptr_t>(rerevved::gpu::guest_d3d::CurrentRingStubServices().get()));
        // The row's own import address selects the entry: the other row's wrapper is not accepted for this row.
        const auto& other = rowOf(info.id == kH ? kW : kH);
        UNSCOPED_INFO(name + ": the other wrapper at the import");
        CHECK(dispatcher->SetFunction(info.importAddress, other.wrapper));
        environment = ProductionVideoImportEnvironmentForTesting(row);
        UNSCOPED_INFO(name + ": the other row's wrapper does not match this row");
        CHECK((environment.target == other.wrapper && environment.target != row.wrapper));
        UNSCOPED_INFO(name + ": restored");
        CHECK(dispatcher->SetFunction(info.importAddress, row.wrapper));

        // A call over the production adapters: no delegate, so the services refusal runs the import.
        auto          entry = makeEntry(info.id, layout);
        std::uint64_t r3    = entry.r3.u64;
        const bool    taken = ReplaceVideoImportStubCall(info.callPC, info.id, makeCall(entry), r3);
        const auto    c     = take(info.id);
        UNSCOPED_INFO(name + ": without a delegate the call is a services refusal");
        CHECK((!taken && r3 == entry.r3.u64 && c.refusal(R::Services) == 1 && c.refusals() == 1 && c.counts.original == 1));
        // The production observers close the record without any fake: no further probe is needed.
        auto ret = atReturn(info.id, entry);
        ObserveVideoImportReturn(info.id, VideoImportObservation{ ret.r1.u64, ret.r3.u64, 0, 0, 0, 0, 0, ret.r11.u64, ret.r29.u64, ret.r30.u64, ret.r31.u64, ret.r13.u64, 0 });
        takeAll();
        ResetVideoImportStateForTesting();
        // A withdrawal ends admission.
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        r3 = entry.r3.u64;
        UNSCOPED_INFO(name + ": an unpublished route is inactive");
        CHECK((!ReplaceVideoImportStubCall(info.callPC, info.id, makeCall(entry), r3) && take(info.id).counts.enter == 0));
    }
    // The production memory read is a checked big-endian load of the runtime's guest memory.
    std::uint32_t word = 0;
    real.put32(frame + 0x1000, 0x12345678);
    UNSCOPED_INFO("a word is read from the runtime's guest memory");
    CHECK((ProductionVideoImportRead32ForTesting(frame + 0x1000, word) && word == 0x12345678));
    UNSCOPED_INFO("an unmapped or wrapping address is unreadable");
    CHECK((!ProductionVideoImportRead32ForTesting(0x1000, word) && !ProductionVideoImportRead32ForTesting(0xFFFFFFFE, word) && !ProductionVideoImportRead32ForTesting(0, word)));
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 22>;

const CaseTable kCases{
    CaseTable::value_type{ "testConstants", testConstants },
    CaseTable::value_type{ "testTable", testTable },
    CaseTable::value_type{ "testDeriveAndApply", testDeriveAndApply },
    CaseTable::value_type{ "testPredicates", testPredicates },
    CaseTable::value_type{ "testRefusalTable", testRefusalTable },
    CaseTable::value_type{ "testFrameIdentity", testFrameIdentity },
    CaseTable::value_type{ "testOriginalRunsOnce", testOriginalRunsOnce },
    CaseTable::value_type{ "testServicesAndLease", testServicesAndLease },
    CaseTable::value_type{ "testDisabledWork", testDisabledWork },
    CaseTable::value_type{ "testExitCheck", testExitCheck },
    CaseTable::value_type{ "testIdentityChanges", testIdentityChanges },
    CaseTable::value_type{ "testObservationPairing", testObservationPairing },
    CaseTable::value_type{ "testBypass", testBypass },
    CaseTable::value_type{ "testDuplicatePost", testDuplicatePost },
    CaseTable::value_type{ "testImportIndependence", testImportIndependence },
    CaseTable::value_type{ "testHsioOwner", testHsioOwner },
    CaseTable::value_type{ "testWorkerOwner", testWorkerOwner },
    CaseTable::value_type{ "testHookAbi", testHookAbi },
    CaseTable::value_type{ "testReport", testReport },
    CaseTable::value_type{ "testLogging", testLogging },
    CaseTable::value_type{ "testRuntimeParity", testRuntimeParity },
    CaseTable::value_type{ "testRuntimeAdmission", testRuntimeAdmission }
};

// A case that never returns is a failure of that case, not a hang. The scope
// ends the watcher thread when the case returns.
class ScopedWatchdog
{
public:
    explicit ScopedWatchdog(std::chrono::seconds limit)
    : thread_([this, limit]
              {
                  std::unique_lock<std::mutex> lock(mutex_);
                  if (!done_.wait_for(lock, limit, [this]
                                      {
                                          return finished_;
                                      }))
                  {
                      std::cerr << "FAILED: " << current_ << ": the case did not finish within " << limit.count() << " seconds" << std::endl;
                      std::_Exit(3);
                  }
              })
    {
    }

    ~ScopedWatchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        done_.notify_one();
        thread_.join();
    }

    ScopedWatchdog(const ScopedWatchdog&)            = delete;
    ScopedWatchdog& operator=(const ScopedWatchdog&) = delete;

    void SetCase(const char* name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = name;
    }

private:
    std::mutex              mutex_;
    std::condition_variable done_;
    bool                    finished_ = false;
    std::string             current_;
    std::thread             thread_;
};

// Runs every table entry, or only the entry named `selected`.
unsigned runCases(std::string_view selected)
{
    ScopedWatchdog watchdog(std::chrono::seconds(90));
    unsigned       ran = 0;
    for (const auto& [name, run] : kCases)
        if (selected.empty() || selected == name)
        {
            watchdog.SetCase(name);
            INFO("case " << name);
            run();
            ++ran;
        }
    return ran;
}

} // namespace

TEST_CASE("video_import_stub_table", "[ctest][video-import-stub-table][timeout-120]")
{
    runCases({});
}

// Runs one named case:
//   video-import-stub-table.case <testName>
TEST_CASE("video_import_stub_table_selected", "[.][manual][video-import-stub-table]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(videoImportStubTableCase, "video-import-stub-table.case", "video_import_stub_table_selected")
