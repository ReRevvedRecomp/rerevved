#include "allocation_harness.hpp"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/video_display_mode_stub.h"
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
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

REXCVAR_DECLARE(bool, native_video_display_mode_stub);
REXCVAR_DECLARE(bool, native_video_display_mode_exit_check);

// The hook adapters the generated code calls (video_display_mode_hooks.cpp).
bool ReplaceNativeVideoDisplayModeCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r31, uint64_t lr);
void ObserveNativeVideoDisplayModeReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r5, PPCRegister& r31);
void ObserveNativeVideoDisplayModeReconverged(PPCRegister& r1, PPCRegister& r3, PPCRegister& r5, PPCRegister& r31, uint64_t lr);

// The SDK exports the owner calls.
extern "C" void __imp__VdSetDisplayMode(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__VdGetCurrentDisplayInformation(PPCContext& ctx, uint8_t* base);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using rex::X_STATUS;
using R = VideoDisplayModeRefusal;
using M = VideoDisplayModeMismatch;

constexpr std::uint32_t kPoison = 0xDEADBEEF, kEntryLr = 0x826A4900, kNextLr = 0x826A626C;
constexpr std::uint64_t kCallerR31 = 0xCA11000000003100ULL;

// One owner invocation's addresses: its stack pointer after the prologue and the
// device G. The caller's frame starts 192 bytes above.
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

VideoDisplayModeEnvironment goodEnvironment()
{
    VideoDisplayModeEnvironment env;
    env.runtime           = true;
    env.graphics          = false;
    env.delegate          = true;
    env.dispatcher        = true;
    env.targetPresent     = true;
    env.targetMatches     = true;
    env.identity.runtime  = 0x1001;
    env.identity.delegate = 0x2002;
    env.identity.ring     = 0x3003;
    env.identity.target   = 0x4004;
    return env;
}

// A guest address space with checked reads, the environment probe and the log.
class Guest
{
public:
    std::mutex                            mutex;
    std::map<std::uint32_t, std::uint8_t> bytes;
    std::vector<Region>                   regions;
    std::set<std::uint32_t>               unreadable;
    std::atomic<unsigned>                 reads{ 0 }, environments{ 0 };
    std::vector<std::string>              logs;
    bool                                  throwOnLog = false;
    VideoDisplayModeEnvironment           env        = goodEnvironment();
    std::function<void(unsigned)>         onEnvironment;
    std::function<void()>                 onCommit;
    VideoDisplayModeOperations            ops;
    bool                                  installed = false;

    Guest()                        = default;
    Guest(const Guest&)            = delete;
    Guest& operator=(const Guest&) = delete;

    ~Guest()
    {
        if (installed)
            SetVideoDisplayModeOperationsForTesting(nullptr);
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

    void add(Region region)
    {
        std::lock_guard lock(mutex);
        regions.push_back(region);
    }

    void mutateEnvironment(const std::function<void(VideoDisplayModeEnvironment&)>& change)
    {
        std::lock_guard lock(mutex);
        change(env);
    }

    VideoDisplayModeMemory memory()
    {
        VideoDisplayModeMemory memory;
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

    void install()
    {
        ops = VideoDisplayModeOperations{ memory(),
                                          [this]
                                          {
                                              const auto index = environments.fetch_add(1) + 1;
                                              if (onEnvironment)
                                                  onEnvironment(static_cast<unsigned>(index));
                                              std::lock_guard lock(mutex);
                                              return env;
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
        SetVideoDisplayModeOperationsForTesting(&ops);
    }

    std::vector<std::string> logLines()
    {
        std::lock_guard lock(mutex);
        return logs;
    }
};

// The owner's input structure sits inside its frame region.
constexpr std::uint32_t kInput = 0x800;

struct Mode
{
    std::uint32_t width = 1280, height = 720, modeWord = 1, colorWord = 0;
};

// An independent oracle of the owner's flags (G:12660-12708).
std::uint64_t expectedFlags(const Mode& m)
{
    std::uint32_t flags  = 0x40000000 | ((m.colorWord & 3U) << 28);
    const auto    format = m.modeWord & 0x3FU;
    if (format == 7 || format == 54)
        flags |= 0x08000000;
    if (m.width == 720 && (m.height == 480 || m.height == 576))
        flags |= 2;
    return flags;
}

void putMode(Guest& guest, const Layout& l, const Mode& m)
{
    guest.put32(l.sp + kInput + 0, m.width);
    guest.put32(l.sp + kInput + 4, m.height);
    guest.put32(l.sp + kInput + 64, m.modeWord);
    guest.put32(l.sp + kInput + 68, m.colorWord);
}

void setup(Guest& guest, const Layout& l, const Mode& mode = {})
{
    guest.add({ l.sp - 0x1000, 0x3000 });
    if (!guest.readable(l.g))
        guest.add({ l.g, 0x6000 });
    for (std::uint32_t offset = 0; offset < 192; offset += 4)
        guest.put32(l.sp + offset, 0x5A000000 | offset);
    guest.put32(l.sp, l.sp + 192); // the backchain the owner's stwu wrote
    for (std::uint32_t offset = 80; offset < 80 + 88; offset += 4)
        guest.put32(l.sp + offset, kPoison);
    for (std::uint32_t offset = 0; offset < 0x5440; offset += 4)
        guest.put32(l.g + offset, 0x64000000 | offset);
    putMode(guest, l, mode);
}

struct Cvars
{
    Cvars(bool stub, bool check, bool published, std::uint32_t validated = L0.g)
    {
        REXCVAR_SET(native_video_display_mode_stub, stub);
        REXCVAR_SET(native_video_display_mode_exit_check, check);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(published ? validated : 0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(published);
        (void)TakeVideoDisplayModeStubCounts();
        ResetVideoDisplayModeLogBudgetForTesting();
    }

    ~Cvars()
    {
        REXCVAR_SET(native_video_display_mode_stub, false);
        REXCVAR_SET(native_video_display_mode_exit_check, false);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        SetVideoDisplayModePendingLimitForTesting(kVideoDisplayModeMaxPending);
    }
};

// The registers at the hook at 0x826A6260: SP after the prologue, G in r31 and the
// flags in r3, every other register poisoned.
PPCContext makeEntryContext(const Layout& l, std::uint64_t flags)
{
    PPCContext ctx{};
    std::memset(&ctx, 0x35, sizeof(ctx));
    ctx.r1.u64  = l.sp;
    ctx.r3.u64  = flags;
    ctx.r4.u64  = l.sp + 192 + 0x40;
    ctx.r5.u64  = 0x7777000000000001ULL;
    ctx.r31.u64 = l.g;
    ctx.lr      = kEntryLr;
    return ctx;
}

// Every register; the struct's tail padding is not guest state and is not copied faithfully.
bool sameContext(const PPCContext& a, const PPCContext& b)
{
    constexpr std::size_t kState = offsetof(PPCContext, v127) + sizeof(PPCVRegister);
    return std::memcmp(&a, &b, kState) == 0;
}

struct Counts
{
    VideoDisplayModeStubCounts counts;

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
};

Counts take()
{
    return { TakeVideoDisplayModeStubCounts() };
}

// Counts and every field that proves a call that ran its import once and was never consumed.
bool isOriginalOnce(const Counts& c)
{
    return c.counts.enter == 1 && c.counts.original == 1 && c.counts.consumed == 0;
}

// Ends the thread's pairing record of a call at `ctx` the way the generated code
// does after the import: the return observation and the reconvergence.
void finishCall(const PPCContext& entry)
{
    auto ctx   = entry;
    ctx.r3.u64 = 0;
    ObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);
    ctx.r3.u64 = ctx.r1.u64 + 80;
    ObserveNativeVideoDisplayModeReconverged(ctx.r1, ctx.r3, ctx.r5, ctx.r31, kNextLr);
}

struct Attempt
{
    bool       replaced = false;
    unsigned   reads    = 0; // guest reads made by the hook itself
    PPCContext before, after;
    Counts     counts;
};

// One call of the production adapter, then the observations that close its record.
Attempt attempt(PPCContext ctx, const std::atomic<unsigned>* reads = nullptr)
{
    Attempt a;
    a.before   = ctx;
    a.replaced = ReplaceNativeVideoDisplayModeCall(ctx.r1, ctx.r3, ctx.r31, ctx.lr);
    a.after    = ctx;
    a.reads    = reads ? reads->load() : 0;
    a.counts   = take();
    finishCall(ctx);
    (void)take();
    return a;
}

// ---------------------------------------------------------------------------
// The owner: the generated sub_826A61F0 (G:rerevved_recomp.221.cpp) over a guest backend.

struct Backend
{
    std::function<std::uint32_t(std::uint32_t)>       get32;
    std::function<void(std::uint32_t, std::uint32_t)> put32;
    std::function<void(PPCContext&)>                  setMode; // bl VdSetDisplayMode
    std::function<void(PPCContext&)>                  getInfo; // bl VdGetCurrentDisplayInformation
};

struct Model
{
    std::uint32_t width = 1280, height = 720;
    unsigned      setModeFault = 0; // 1: the import returns 1
    unsigned      infoFault    = 0; // 1: r3 is clobbered, 2: LR is clobbered
};

std::array<std::uint8_t, 88> modelInfo(std::uint16_t width, std::uint16_t height)
{
    std::array<std::uint8_t, 88> info{};
    for (std::size_t i = 0; i < info.size(); ++i)
        info[i] = static_cast<std::uint8_t>(0x40 + i % 7);
    const auto put16 = [&](std::size_t offset, std::uint16_t value)
    {
        info[offset]     = static_cast<std::uint8_t>(value >> 8);
        info[offset + 1] = static_cast<std::uint8_t>(value);
    };
    put16(0, width);
    put16(2, height);
    put16(72, width);
    put16(74, height);
    put16(86, width);
    return info;
}

Backend fakeBackend(Guest& guest, const Model& model = {})
{
    Backend io;
    io.get32 = [&guest](std::uint32_t address)
    {
        return guest.get32(address);
    };
    io.put32 = [&guest](std::uint32_t address, std::uint32_t value)
    {
        guest.put32(address, value);
    };
    io.setMode = [model](PPCContext& ctx)
    {
        ctx.r3.u64 = model.setModeFault == 1 ? 1 : 0;
    };
    io.getInfo = [&guest, model](PPCContext& ctx)
    {
        const auto info = modelInfo(static_cast<std::uint16_t>(model.width), static_cast<std::uint16_t>(model.height));
        for (std::uint32_t i = 0; i < 88; i += 4)
            guest.put32(ctx.r3.u32 + i, (std::uint32_t{ info[i] } << 24) | (std::uint32_t{ info[i + 1] } << 16) | (std::uint32_t{ info[i + 2] } << 8) | info[i + 3]);
        if (model.infoFault == 1)
            ctx.r3.u64 = 0;
        if (model.infoFault == 2)
            ctx.lr = 0x1234;
    };
    return io;
}

struct Opts
{
    bool                             hooks = true;
    std::uint32_t                    r5    = 1;
    std::function<void(PPCContext&)> tweak;            // after the prologue
    std::function<void()>            pauseReturn;      // at 0x826A6264, before the return observation
    std::function<void()>            pauseReconverged; // at 0x826A626C, before the reconvergence observation
};

struct Owner
{
    PPCContext                 atEntry{}, atHook{}, afterHook{}, atReturn{}, atReconverged{}, final{};
    std::vector<std::uint64_t> setModeCalls;
    std::vector<std::uint64_t> infoCalls;
    std::string                path;
    std::uint64_t              flagsAtHook = 0;
    unsigned                   hookCalls   = 0;
    bool                       replaced    = false;
    std::uint32_t              callerSp    = 0;
    std::uint32_t              stores[3]{};
};

std::uint16_t get16(const Backend& io, std::uint32_t address)
{
    const auto word = io.get32(address & ~3U);
    return static_cast<std::uint16_t>((address & 2U) ? word : word >> 16);
}

Owner runOwner(const Backend& io, const Layout& l, const Mode&, const Opts& opts = {})
{
    Owner owner;
    auto  ctx = makeEntryContext(l, 0);
    // The caller's frame and the caller's registers before the call.
    owner.callerSp = l.sp + 192;
    ctx.r1.u64     = owner.callerSp;
    ctx.r3.u64     = l.g;
    ctx.r4.u64     = l.sp + kInput;
    ctx.r5.u64     = opts.r5;
    ctx.r31.u64    = kCallerR31;
    // mflr r12; stw r12,-8(r1); std r31,-16(r1); stwu r1,-192(r1); mr r31,r3.
    ctx.r12.u64 = ctx.lr;
    io.put32(ctx.r1.u32 - 8, ctx.r12.u32);
    io.put32(ctx.r1.u32 - 16, static_cast<std::uint32_t>(ctx.r31.u64 >> 32));
    io.put32(ctx.r1.u32 - 12, static_cast<std::uint32_t>(ctx.r31.u64));
    io.put32(ctx.r1.u32 - 192, ctx.r1.u32);
    ctx.r1.u64  = ctx.r1.u32 - 192;
    ctx.r31.u64 = ctx.r3.u64;
    if (opts.tweak)
        opts.tweak(ctx);
    owner.atEntry = ctx;

    if (ctx.r5.s32 == 0)
        owner.path = "bypass";
    else
    {
        // The flags: lwz 64(r4), the format test, lwz 68(r4), rlwimi, lwz 0(r4) and the size tests.
        auto r11 = io.get32(ctx.r4.u32 + 64) & 0x3FU;
        auto r10 = std::uint32_t{ 0 };
        if (r11 == 7 || r11 == 54)
            r10 = 0x08000000;
        r11                    = io.get32(ctx.r4.u32 + 68);
        std::uint32_t r9       = 0x40000000;
        r9                     = ((r11 << 28) & 0x30000000U) | (r9 & 0xCFFFFFFFU);
        const std::uint32_t r8 = io.get32(ctx.r4.u32 + 0);
        ctx.r3.u64             = r9 | r10;
        bool special           = false;
        owner.path             = "direct-width";
        if (r8 == 720)
        {
            const auto height = io.get32(ctx.r4.u32 + 4);
            special           = height == 480 || height == 576;
            owner.path        = special ? "special" : "direct-height";
        }
        if (special)
            ctx.r3.u64 |= 2;
        // loc_826A6260.
        owner.flagsAtHook = ctx.r3.u64;
        owner.atHook      = ctx;
        owner.hookCalls   = 1;
        owner.replaced    = opts.hooks && ReplaceNativeVideoDisplayModeCall(ctx.r1, ctx.r3, ctx.r31, ctx.lr);
        owner.afterHook   = ctx;
        if (!owner.replaced)
        {
            ctx.lr = 0x826A6264;
            owner.setModeCalls.push_back(ctx.r3.u64);
            io.setMode(ctx);
        }
    }
    // loc_826A6264.
    // LR differs here by design and is never compared: the snapshot carries none.
    owner.atReturn    = ctx;
    owner.atReturn.lr = 0;
    if (opts.pauseReturn)
        opts.pauseReturn();
    if (opts.hooks)
        ObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);
    ctx.r3.s64 = ctx.r1.s64 + 80;
    ctx.lr     = 0x826A626C;
    owner.infoCalls.push_back(ctx.r3.u64);
    io.getInfo(ctx);
    owner.atReconverged = ctx;
    if (opts.pauseReconverged)
        opts.pauseReconverged();
    if (opts.hooks)
        ObserveNativeVideoDisplayModeReconverged(ctx.r1, ctx.r3, ctx.r5, ctx.r31, ctx.lr);
    // lhz 152, 154 and 166, then the three stores into G.
    ctx.r11.u64 = get16(io, ctx.r1.u32 + 152);
    ctx.r10.u64 = get16(io, ctx.r1.u32 + 154);
    ctx.r9.u64  = get16(io, ctx.r1.u32 + 166);
    io.put32(ctx.r31.u32 + 21544, ctx.r11.u32);
    io.put32(ctx.r31.u32 + 21548, ctx.r10.u32);
    io.put32(ctx.r31.u32 + 21552, ctx.r9.u32);
    owner.stores[0] = ctx.r31.u32 + 21544;
    owner.stores[1] = ctx.r31.u32 + 21548;
    owner.stores[2] = ctx.r31.u32 + 21552;
    // The epilogue: addi r1,r1,192; lwz r12,-8(r1); mtlr r12; ld r31,-16(r1).
    ctx.r1.s64  = ctx.r1.s64 + 192;
    ctx.r12.u64 = io.get32(ctx.r1.u32 - 8);
    ctx.lr      = ctx.r12.u64;
    ctx.r31.u64 = (std::uint64_t{ io.get32(ctx.r1.u32 - 16) } << 32) | io.get32(ctx.r1.u32 - 12);
    owner.final = ctx;
    return owner;
}

Owner runFake(Guest& guest, const Layout& l, const Mode& mode = {}, const Opts& opts = {}, const Model& model = {})
{
    return runOwner(fakeBackend(guest, model), l, mode, opts);
}

// ---------------------------------------------------------------------------

void testConstants()
{
    static_assert(kVideoDisplayModeHookAddress == 0x826A6260 && kVideoDisplayModeReturnAddress == 0x826A6264 && kVideoDisplayModeReconvergedAddress == 0x826A626C);
    static_assert(kVideoDisplayModeImportAddress == 0x82E924F4 && kVideoDisplayModeFrameBytes == 192 && kVideoDisplayModeInfoOffset == 80 && kVideoDisplayModeInfoBytes == 88);
    static_assert(kVideoDisplayModeRequiredFlag == 0x40000000 && kVideoDisplayModeOptionalFlags == 0x38000002 && (kVideoDisplayModeRequiredFlag & kVideoDisplayModeOptionalFlags) == 0);
    static_assert(kVideoDisplayModeInfoOffset + kVideoDisplayModeWidthOffset == 152 && kVideoDisplayModeInfoOffset + kVideoDisplayModeHeightOffset == 154 &&
                  kVideoDisplayModeInfoOffset + kVideoDisplayModeActualWidthOffset == 166);
    UNSCOPED_INFO("seven refusal reasons and three mismatch kinds");
    CHECK((kVideoDisplayModeRefusalCount == 7 && kVideoDisplayModeMismatchCount == 3));
    const char* refusals[]   = { "services", "provider", "diagnostics", "stack", "shape", "reentrant", "unpaired" };
    const char* mismatches[] = { "return", "continuation", "lr" };
    for (std::size_t i = 0; i < 7; ++i)
    {
        UNSCOPED_INFO("refusal name order");
        CHECK(kVideoDisplayModeRefusalNames[i] == refusals[i]);
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        UNSCOPED_INFO("mismatch name order");
        CHECK(kVideoDisplayModeMismatchNames[i] == mismatches[i]);
    }
    UNSCOPED_INFO("both options default to off");
    CHECK((!REXCVAR_GET(native_video_display_mode_stub) && !REXCVAR_GET(native_video_display_mode_exit_check)));
    UNSCOPED_INFO("the final drain waits at most 10 ms");
    CHECK(kVideoDisplayModeShutdownWait == std::chrono::milliseconds(10));
    UNSCOPED_INFO("64 outstanding calls at most");
    CHECK(kVideoDisplayModeMaxPending == 64);
}

VideoDisplayModeState goodState()
{
    VideoDisplayModeState state;
    state.lease       = true;
    state.ready       = true;
    state.diagnostics = false;
    state.environment = goodEnvironment();
    return state;
}

VideoDisplayModeCall makeCall(const PPCContext& ctx)
{
    VideoDisplayModeCall call;
    call.r1  = ctx.r1.u64;
    call.r3  = ctx.r3.u64;
    call.r31 = ctx.r31.u64;
    call.lr  = ctx.lr;
    return call;
}

void testDeriveAndApply()
{
    Guest guest;
    setup(guest, L0);
    const auto           memory = guest.memory();
    const auto           before = guest.bytes;
    const auto           call   = makeCall(makeEntryContext(L0, 0x40000000));
    VideoDisplayModePlan plan;
    const auto           refusal = DeriveVideoDisplayModeStub(memory, call, goodState(), plan);
    UNSCOPED_INFO("the owner's call derives");
    CHECK(!refusal);
    UNSCOPED_INFO("the plan keeps the full entry registers");
    CHECK((plan.r1 == L0.sp && plan.r3 == 0x40000000 && plan.r31 == L0.g && plan.lr == kEntryLr && plan.sp == L0.sp && plan.device == L0.g));
    UNSCOPED_INFO("the plan binds the identities");
    CHECK(plan.identity == goodEnvironment().identity);
    UNSCOPED_INFO("derive reads only the backchain");
    CHECK(guest.reads == 1);
    UNSCOPED_INFO("derive changes no guest byte");
    CHECK(guest.bytes == before);
    VideoDisplayModePlan again;
    UNSCOPED_INFO("derive is repeatable");
    CHECK((!DeriveVideoDisplayModeStub(memory, call, goodState(), again) && again.sp == plan.sp && again.identity == plan.identity));

    // The apply seam clears the full 64 bits whatever they held.
    for (const std::uint64_t value : std::initializer_list<std::uint64_t>{ std::uint64_t{ 0x40000000 }, 0xFFFFFFFF00000040ULL, 0x00000001FFFFFFFFULL, ~0ULL, 0ULL })
    {
        auto r3 = value;
        ApplyVideoDisplayModeStub(plan, r3);
        UNSCOPED_INFO("apply assigns the full r3 = 0");
        CHECK(r3 == 0);
    }
}

void testFlagShapes()
{
    Guest guest;
    setup(guest, L0);
    guest.install();
    Cvars cvars(true, false, true);
    // The sixteen combinations of the optional bits 27, 28, 29 and 1, each with bit 30.
    const std::uint32_t optional[] = { 0x08000000, 0x10000000, 0x20000000, 0x2 };
    for (unsigned combo = 0; combo < 16; ++combo)
    {
        std::uint32_t flags = 0x40000000;
        for (unsigned bit = 0; bit < 4; ++bit)
            if (combo & (1U << bit))
                flags |= optional[bit];
        const auto before = guest.bytes;
        const auto a      = attempt(makeEntryContext(L0, flags));
        UNSCOPED_INFO("a valid combination is consumed with the full r3 = 0");
        CHECK((a.replaced && a.after.r3.u64 == 0));
        UNSCOPED_INFO("consumed once, no refusal");
        CHECK((a.counts.counts.enter == 1 && a.counts.counts.consumed == 1 && a.counts.counts.original == 0 && a.counts.refusals() == 0));
        auto expected   = a.before;
        expected.r3.u64 = 0;
        UNSCOPED_INFO("only r3 changed");
        CHECK(sameContext(a.after, expected));
        UNSCOPED_INFO("no guest byte changed");
        CHECK(guest.bytes == before);
    }
    // Every bit outside the sixteen combinations and bit 30 refuses.
    for (unsigned bit = 0; bit < 32; ++bit)
    {
        const std::uint32_t one = 1U << bit;
        if (one == 0x40000000 || (one & kVideoDisplayModeOptionalFlags))
            continue;
        const auto a = attempt(makeEntryContext(L0, 0x40000000 | one));
        UNSCOPED_INFO("a forbidden bit leaves the context untouched");
        CHECK((!a.replaced && sameContext(a.before, a.after)));
        UNSCOPED_INFO("a forbidden bit is one shape refusal");
        CHECK((a.counts.refusal(R::Shape) == 1 && a.counts.refusals() == 1 && isOriginalOnce(a.counts)));
    }
    // Bit 30 is required whatever else is set.
    for (unsigned combo = 0; combo < 16; ++combo)
    {
        std::uint32_t flags = 0;
        for (unsigned bit = 0; bit < 4; ++bit)
            if (combo & (1U << bit))
                flags |= optional[bit];
        const auto a = attempt(makeEntryContext(L0, flags));
        UNSCOPED_INFO("a missing bit 30 is a shape refusal");
        CHECK((!a.replaced && a.counts.refusal(R::Shape) == 1 && a.counts.refusals() == 1));
    }
    // Nothing is normalised: a high word, even with valid low flags, refuses.
    for (const std::uint64_t flags : { 0x0000000140000000ULL, 0xFFFFFFFF40000000ULL, 0x8000000040000000ULL, 0x00000001FFFFFFFFULL })
    {
        const auto a = attempt(makeEntryContext(L0, flags));
        UNSCOPED_INFO("a high word is not masked off");
        CHECK((!a.replaced && sameContext(a.before, a.after)));
        UNSCOPED_INFO("a high word is one shape refusal");
        CHECK((a.counts.refusal(R::Shape) == 1 && a.counts.refusals() == 1 && isOriginalOnce(a.counts)));
    }
}

void testRefusalTable()
{
    struct Case
    {
        const char*                              name;
        R                                        reason;
        bool                                     check;
        std::function<void(Guest&, PPCContext&)> tweak;
    };

    const auto addFrame = [](Guest& g, std::uint32_t sp)
    {
        g.add({ sp & ~0xFFFU, 0x1000 });
        g.put32(sp, sp + 192);
    };
    const Case cases[] = {
        { "device not validated", R::Services, false, [](Guest&, PPCContext&)
          {
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(L0.g + 4);
          } },
        { "graphics present", R::Services, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.graphics = true;
                                  });
          } },
        { "delegate missing", R::Services, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.delegate          = false;
                                      e.identity.delegate = 0;
                                  });
          } },
        { "runtime missing", R::Services, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e = {};
                                  });
          } },
        { "dispatcher missing", R::Provider, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.dispatcher    = false;
                                      e.targetPresent = false;
                                      e.targetMatches = false;
                                  });
          } },
        { "target missing", R::Provider, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.targetPresent   = false;
                                      e.targetMatches   = false;
                                      e.identity.target = 0;
                                  });
          } },
        { "target is not the retained wrapper", R::Provider, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.targetMatches = false;
                                  });
          } },
        { "exit check forces the import", R::Diagnostics, true, nullptr },
        { "SP misaligned", R::Stack, false, [](Guest& g, PPCContext& c)
          {
              g.put32(L0.sp + 8, L0.sp + 8 + 192);
              c.r1.u64 = L0.sp + 8;
          } },
        { "SP below 0x10000", R::Stack, false, [&addFrame](Guest& g, PPCContext& c)
          {
              addFrame(g, 0xFFF0);
              c.r1.u64 = 0xFFF0;
          } },
        { "SP frame wraps", R::Stack, false, [&addFrame](Guest& g, PPCContext& c)
          {
              g.add({ 0xFFFFF000, 0x1000 });
              g.put32(0xFFFFFF40, 0xFFFFFF40U + 192U);
              c.r1.u64 = 0xFFFFFF40;
          } },
        { "r1 high word set", R::Stack, false, [](Guest&, PPCContext& c)
          {
              c.r1.u64 = (std::uint64_t{ 1 } << 32) | L0.sp;
          } },
        { "backchain wrong", R::Stack, false, [](Guest& g, PPCContext&)
          {
              g.put32(L0.sp, L0.sp + 192 + 16);
          } },
        { "backchain unreadable", R::Stack, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp);
          } },
        { "flags shape", R::Shape, false, [](Guest&, PPCContext& c)
          {
              c.r3.u64 = 0x40000004;
          } },
        { "no observation record", R::Unpaired, false, [](Guest&, PPCContext&)
          {
              SetVideoDisplayModePendingLimitForTesting(0);
          } },
        // Several failures at once: the first reason in the order wins.
        { "services before provider and shape", R::Services, false, [](Guest& g, PPCContext& c)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.delegate      = false;
                                      e.targetMatches = false;
                                  });
              c.r3.u64 = 0;
          } },
        { "provider before diagnostics and stack", R::Provider, true, [](Guest& g, PPCContext& c)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.targetMatches = false;
                                  });
              c.r1.u64 = L0.sp + 8;
          } },
        { "diagnostics before stack and shape", R::Diagnostics, true, [](Guest&, PPCContext& c)
          {
              c.r1.u64 = L0.sp + 8;
              c.r3.u64 = 0;
          } },
        { "stack before shape", R::Stack, false, [](Guest&, PPCContext& c)
          {
              c.r1.u64 = L0.sp + 8;
              c.r3.u64 = 0;
          } },
        { "unreadable backchain before shape", R::Stack, false, [](Guest& g, PPCContext& c)
          {
              g.unreadable.insert(L0.sp);
              c.r3.u64 = 0;
          } },
    };
    for (const auto& c : cases)
    {
        const std::string name = c.name;
        Guest             guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, c.check, true);
        auto  ctx = makeEntryContext(L0, 0x40000000);
        if (c.tweak)
            c.tweak(guest, ctx);
        const auto bytes = guest.bytes;
        const auto a     = attempt(ctx, &guest.reads);
        UNSCOPED_INFO(name + ": not replaced");
        CHECK(!a.replaced);
        UNSCOPED_INFO(name + ": the hook changes no register");
        CHECK(sameContext(a.before, a.after));
        UNSCOPED_INFO(name + ": the slice changes no guest byte");
        CHECK(guest.bytes == bytes);
        UNSCOPED_INFO(name + ": exactly that refusal");
        CHECK((a.counts.refusal(c.reason) == 1 && a.counts.refusals() == 1));
        UNSCOPED_INFO(name + ": one entry, one original, no consume");
        CHECK(isOriginalOnce(a.counts));
        if (c.reason == R::Services || c.reason == R::Provider || c.reason == R::Diagnostics)
        {
            UNSCOPED_INFO(name + ": the decision reads no guest word");
            CHECK(a.reads == 0);
        }
        SetVideoDisplayModePendingLimitForTesting(kVideoDisplayModeMaxPending);
    }
    // Refusals decided before the frame is examined read no guest word for the decision.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        guest.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                {
                                    e.delegate = false;
                                });
        auto          ctx      = makeEntryContext(L0, 0x40000000);
        std::uint64_t r3       = ctx.r3.u64;
        const auto    replaced = ReplaceVideoDisplayModeCall(makeCall(ctx), r3);
        UNSCOPED_INFO("a services refusal reads nothing");
        CHECK((!replaced && guest.reads == 0));
        (void)take();
        finishCall(ctx);
        (void)take();
    }
}

void testOriginalRunsOnce()
{
    struct Case
    {
        const char*                              name;
        bool                                     stub, check;
        std::function<void(Guest&, PPCContext&)> tweak;
        bool                                     consumed;
    };

    const Case cases[] = {
        { "consumed", true, false, nullptr, true },
        { "services refused", true, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.graphics = true;
                                  });
          },
          false },
        { "provider refused", true, false, [](Guest& g, PPCContext&)
          {
              g.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                  {
                                      e.targetMatches = false;
                                  });
          },
          false },
        // Only the stub reads the owner backchain word.
        { "stack refused", true, false, [](Guest& g, PPCContext&)
          {
              g.put32(L0.sp, 0);
          },
          false },
        { "exit check alone", false, true, nullptr, false },
        { "both options", true, true, nullptr, false },
    };
    for (const auto& c : cases)
    {
        const std::string name = c.name;
        Guest             guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(c.stub, c.check, true);
        const auto owner  = runFake(guest, L0, Mode{}, Opts{ .tweak = [&](PPCContext& ctx)
                                                             {
                                                                if (c.tweak)
                                                                    c.tweak(guest, ctx);
                                                             } });
        const auto counts = take();
        UNSCOPED_INFO(name + ": consumed exactly when expected");
        CHECK(owner.replaced == c.consumed);
        UNSCOPED_INFO(name + ": SetDisplayMode exactly once, or zero when consumed");
        CHECK(owner.setModeCalls.size() == (c.consumed ? 0U : 1U));
        if (!c.consumed && owner.setModeCalls.size() == 1)
        {
            UNSCOPED_INFO(name + ": the import sees the owner's flags");
            CHECK((owner.setModeCalls[0] == owner.flagsAtHook && owner.flagsAtHook == expectedFlags(Mode{})));
        }
        UNSCOPED_INFO(name + ": the display-information import runs once with SP+80");
        CHECK((owner.infoCalls.size() == 1 && owner.infoCalls[0] == L0.sp + 80));
        UNSCOPED_INFO(name + ": enter == consumed + original");
        CHECK((counts.counts.enter == 1 && counts.counts.consumed == (c.consumed ? 1U : 0U) && counts.counts.original == (c.consumed ? 0U : 1U)));
        if (c.check)
        {
            UNSCOPED_INFO(name + ": the exit check wins over consume");
            CHECK(counts.counts.consumed == 0);
        }
    }
}

void testServicesAndLease()
{
    // Unpublished services with the stub on: inactive, nothing is counted or read.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, false, false);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("no published services: the import runs");
        CHECK((!owner.replaced && owner.setModeCalls.size() == 1));
        UNSCOPED_INFO("unpublished services count and read nothing");
        CHECK((counts.counts.enter == 0 && counts.refusals() == 0 && counts.counts.original == 0 && counts.counts.exitUnpaired == 0 && guest.reads == 0 && guest.environments == 0));
    }
    // A withdrawal between the cheap test and the lease runs the import.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        SetRingStubLeaseProbe([]() noexcept
                              {
                                  rerevved::gpu::guest_d3d::ringStubsActive.store(false, std::memory_order_seq_cst);
                              });
        const auto owner = runFake(guest, L0);
        SetRingStubLeaseProbe(nullptr);
        const auto counts = take();
        UNSCOPED_INFO("a withdrawal before the lease runs the import");
        CHECK((!owner.replaced && owner.setModeCalls.size() == 1));
        UNSCOPED_INFO("counted as services");
        CHECK((counts.refusal(R::Services) == 1 && counts.refusals() == 1 && counts.counts.consumed == 0 && counts.counts.original == 1));
        UNSCOPED_INFO("no environment is read without the lease");
        CHECK(guest.environments == 0);
    }
    // A withdrawal cannot finish while the admitted entry holds its lease.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars              cvars(true, false, true);
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
                               owner = runFake(guest, L0);
                           });
        reached.get_future().wait();
        std::thread withdrawer([&]
                               {
                                   rerevved::gpu::guest_d3d::WithdrawRingStubServices();
                                   withdrawn = true;
                               });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        UNSCOPED_INFO("the withdrawal waits for the entry's lease");
        CHECK(!withdrawn.load());
        release.set_value();
        writer.join();
        withdrawer.join();
        UNSCOPED_INFO("the withdrawal finishes after the entry");
        CHECK(withdrawn.load());
        UNSCOPED_INFO("the admitted entry finished its consume before the withdrawal returned");
        CHECK((owner.replaced && owner.setModeCalls.empty()));
        const auto counts = take();
        // The withdrawal moved the ring identity? The injected probe keeps it; the pair stays valid.
        UNSCOPED_INFO("one consume, nothing outstanding");
        CHECK((counts.counts.consumed == 1 && counts.counts.pending == 0));
    }
    // The lease is still held when the full r3 is assigned.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars             cvars(true, false, true);
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
            UNSCOPED_INFO("a withdrawal cannot finish before the commit");
            CHECK(!withdrawn.load());
        };
        const auto owner = runFake(guest, L0);
        withdrawer.join();
        UNSCOPED_INFO("the consume committed and the withdrawal finished after it");
        CHECK((owner.replaced && withdrawn.load()));
        (void)take();
    }
    // The lease ends before Original runs: a withdrawal after a refused entry does not wait.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars         cvars(true, false, true);
        auto          ctx = makeEntryContext(L0, 0x40000000 | 0x100);
        std::uint64_t r3  = ctx.r3.u64;
        UNSCOPED_INFO("refused");
        CHECK(!ReplaceVideoDisplayModeCall(makeCall(ctx), r3));
        const auto begun = std::chrono::steady_clock::now();
        rerevved::gpu::guest_d3d::WithdrawRingStubServices();
        UNSCOPED_INFO("the lease was released before the import runs");
        CHECK(std::chrono::steady_clock::now() - begun < std::chrono::seconds(2));
        (void)take();
        finishCall(ctx);
        (void)take();
    }
}

void testDisabledWork()
{
    PPCContext ctx = makeEntryContext(L0, 0x40000000);
    Guest      guest;
    setup(guest, L0);
    guest.install();
    for (int mode = 0; mode < 2; ++mode)
    {
        // Mode 0: both options off with services published. Mode 1: the stub on, services not published.
        Cvars         cvars(mode == 1, false, mode == 0);
        const auto    touches   = VideoDisplayModeThreadTouchesForTesting();
        bool          replaced  = false;
        std::uint64_t allocated = 0;
        {
            rerevved::tests::allocation::ScopedAllocationCount count;
            for (int i = 0; i < 1000; ++i)
            {
                replaced |= ReplaceNativeVideoDisplayModeCall(ctx.r1, ctx.r3, ctx.r31, ctx.lr);
                replaced |= ReplaceVideoDisplayModeCall(makeCall(ctx), ctx.r3.u64);
                ObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);
                ObserveNativeVideoDisplayModeReconverged(ctx.r1, ctx.r3, ctx.r5, ctx.r31, kNextLr);
            }
            allocated = count.Count();
        }
        const auto counts = take();
        UNSCOPED_INFO("disabled: nothing is replaced");
        CHECK(!replaced);
        UNSCOPED_INFO("disabled: no allocation");
        CHECK(allocated == 0);
        UNSCOPED_INFO("disabled: no memory access and no admission callback");
        CHECK((guest.reads == 0 && guest.environments == 0));
        UNSCOPED_INFO("disabled: no thread bookkeeping");
        CHECK(VideoDisplayModeThreadTouchesForTesting() == touches);
        UNSCOPED_INFO("disabled: nothing is counted");
        CHECK((counts.counts.enter == 0 && counts.refusals() == 0 && counts.counts.original == 0 && counts.counts.exitUnpaired == 0 && counts.counts.exitOk == 0 && counts.counts.pending == 0 &&
               counts.counts.reentry == 0));
        if (mode == 0)
        {
            UNSCOPED_INFO("disabled: the drains return no fields");
            CHECK((ReportVideoDisplayModeStub().empty() && ReportVideoDisplayModeStub(true).empty()));
        }
        UNSCOPED_INFO("disabled: nothing is logged");
        CHECK(guest.logLines().empty());
        UNSCOPED_INFO("disabled: registers unchanged");
        CHECK(sameContext(ctx, makeEntryContext(L0, 0x40000000)));
    }
}

void testExitCheck()
{
    // Both options: the import is forced to run and the call is observed.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, true, true);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("exit check: the import runs exactly once");
        CHECK((!owner.replaced && owner.setModeCalls.size() == 1));
        UNSCOPED_INFO("exit check: consumed 0, one diagnostics refusal");
        CHECK((counts.counts.consumed == 0 && counts.counts.original == 1 && counts.refusal(R::Diagnostics) == 1 && counts.refusals() == 1));
        UNSCOPED_INFO("exit check: the matched observation is ok");
        CHECK((counts.counts.exitOk == 1 && counts.mismatches() == 0 && counts.counts.exitUnreadable == 0 && counts.counts.exitUnpaired == 0));
    }
    // Both options with a failing earlier prerequisite count that prerequisite, not diagnostics.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        guest.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                {
                                    e.targetMatches = false;
                                });
        Cvars      cvars(true, true, true);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("exit check: the provider refusal wins over diagnostics");
        CHECK((!owner.replaced && counts.refusal(R::Provider) == 1 && counts.refusal(R::Diagnostics) == 0 && counts.counts.consumed == 0));
        UNSCOPED_INFO("an invalid provider is never ok");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
    }
    // The exit check alone observes without any refusal, with the lease and probe of every active entry.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(false, true, true);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("check only: the import runs");
        CHECK((!owner.replaced && owner.setModeCalls.size() == 1));
        UNSCOPED_INFO("check only: exit_ok");
        CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.refusals() == 0 && counts.counts.exitOk == 1));
        UNSCOPED_INFO("check only: nothing outstanding");
        CHECK((counts.counts.pending == 0 && counts.counts.consumed == 0));
        UNSCOPED_INFO("check only: the identities are read at the entry and at the reconvergence");
        CHECK(guest.environments == 2);
    }
    // Without a held lease the provider is not valid: the call is observed but never ok.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(false, true, false);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("check only without services: the import runs");
        CHECK((!owner.replaced && owner.setModeCalls.size() == 1));
        UNSCOPED_INFO("check only without services: no refusal");
        CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.refusals() == 0));
        UNSCOPED_INFO("check only without services: provider_valid is false, so unpaired");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.counts.pending == 0));
        const auto logs = guest.logLines();
        UNSCOPED_INFO("the record says the provider was not valid");
        CHECK((logs.size() == 1 && logs[0].find("provider_valid=0") != std::string::npos && logs[0].find("verdict=unpaired") != std::string::npos));
    }
    // A graphics system or a missing delegate make an observed call inconclusive too.
    for (int variant = 0; variant < 2; ++variant)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        guest.mutateEnvironment([&](VideoDisplayModeEnvironment& e)
                                {
                                    if (variant == 0)
                                        e.graphics = true;
                                    else
                                        e.delegate = false;
                                });
        Cvars cvars(false, true, true);
        (void)runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("an invalid route is never ok");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1));
    }

    struct Fault
    {
        const char* name;
        Model       model;
        M           kind;
    };

    const Fault faults[] = {
        { "wrong result", Model{ .setModeFault = 1 }, M::Return },
        { "clobbered continuation", Model{ .infoFault = 1 }, M::Continuation },
        { "wrong reconverged LR", Model{ .infoFault = 2 }, M::Lr },
    };
    for (const auto& fault : faults)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(false, true, true);
        (void)runFake(guest, L0, Mode{}, Opts{}, fault.model);
        const auto counts = take();
        UNSCOPED_INFO(std::string(fault.name) + ": counted and not ok");
        CHECK((counts.mismatch(fault.kind) == 1 && counts.mismatches() == 1 && counts.counts.exitOk == 0));
    }
    // An unreadable word of the information is inconclusive.
    for (const std::uint32_t offset : { 80U, 80U + 40U, 80U + 84U })
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(false, true, true);
        (void)runFake(guest, L0, Mode{}, Opts{ .pauseReconverged = [&]
                                               {
                                                   guest.unreadable.insert(L0.sp + offset);
                                               } });
        const auto counts = take();
        UNSCOPED_INFO("an unreadable information word is inconclusive");
        CHECK((counts.counts.exitUnreadable == 1 && counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 0));
        const auto logs = guest.logLines();
        UNSCOPED_INFO("the record says the information is unreadable");
        CHECK((logs.size() == 1 && logs[0].find("display_info_valid=0") != std::string::npos && logs[0].find("verdict=unreadable") != std::string::npos));
    }
}

void testIdentityChanges()
{
    enum Which
    {
        kRuntime,
        kDelegate,
        kRing,
        kTarget
    };

    for (const bool stub : { false, true })
        for (const Which which : { kRuntime, kDelegate, kRing, kTarget })
        {
            Guest guest;
            setup(guest, L0);
            guest.install();
            Cvars             cvars(stub, !stub, true);
            const auto        owner  = runFake(guest, L0, Mode{}, Opts{ .pauseReturn = [&]
                                                                        {
                                                                    guest.mutateEnvironment([&](VideoDisplayModeEnvironment& e)
                                                                                            {
                                                                                                auto& id = e.identity;
                                                                                                (which == kRuntime ? id.runtime : which == kDelegate ? id.delegate
                                                                                                                              : which == kRing       ? id.ring
                                                                                                                                                     : id.target) += 1;
                                                                                            });
                                                                        } });
            const auto        counts = take();
            const std::string name   = std::string(stub ? "stub" : "check") + " identity " + std::to_string(static_cast<int>(which));
            UNSCOPED_INFO(name + ": the entry decision was made before the change");
            CHECK(owner.replaced == stub);
            UNSCOPED_INFO(name + ": an identity change is unpaired, never a mismatch or ok");
            CHECK((counts.counts.exitUnpaired == 1 && counts.mismatches() == 0 && counts.counts.exitOk == 0 && counts.counts.exitUnreadable == 0 && counts.counts.pending == 0));
            const auto logs = guest.logLines();
            UNSCOPED_INFO(name + ": the record shows the provider as not stable");
            CHECK((logs.size() == 1 && logs[0].find("provider_valid=0") != std::string::npos && logs[0].find("provider=bad") != std::string::npos));
        }
    // A ring service withdrawn after a consume ends the pair as unpaired.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, false, true);
        const auto owner  = runFake(guest, L0, Mode{}, Opts{ .pauseReturn = [&]
                                                             {
                                                                guest.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                                                                        {
                                                                                            e.identity.ring = 0;
                                                                                        });
                                                             } });
        const auto counts = take();
        UNSCOPED_INFO("a withdrawn ring service ends the pair as unpaired");
        CHECK((owner.replaced && counts.counts.consumed == 1 && counts.counts.exitUnpaired == 1 && counts.counts.exitOk == 0));
    }
    // A stable identity is ok.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        (void)runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a stable identity pairs ok");
        CHECK((counts.counts.exitOk == 1 && counts.counts.exitUnpaired == 0));
    }
}

std::atomic<unsigned>    probeArrived{ 0 };
unsigned                 probeThreads = 0;
std::promise<void>       probeAll;
std::shared_future<void> probeReady;

// Holds every thread between the bound check and the slot reservation until all have passed the check.
void reserveBarrier()
{
    if (probeArrived.fetch_add(1) + 1 == probeThreads)
        probeAll.set_value();
    probeReady.wait();
}

void testObservationPairing()
{
    const auto pausing = [](std::promise<void>& paused, std::shared_future<void> resumed)
    {
        return Opts{ .pauseReturn = [&paused, resumed]
                     {
                         paused.set_value();
                         resumed.wait();
                     } };
    };
    // A drain falls between the boundaries of a call.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars              cvars(true, false, true);
        std::promise<void> paused, resume;
        const auto         resumed = resume.get_future().share();
        std::thread        other([&]
                                 {
                              (void)runFake(guest, L0, Mode{}, pausing(paused, resumed));
                                 });
        paused.get_future().wait();
        const auto mid = take();
        UNSCOPED_INFO("mid-call drain: one consumed, one outstanding");
        CHECK((mid.counts.enter == 1 && mid.counts.consumed == 1 && mid.counts.pending == 1));
        resume.set_value();
        other.join();
        const auto last = take();
        UNSCOPED_INFO("the call paired across the drain");
        CHECK((last.counts.exitOk == 1 && last.counts.pending == 0 && last.mismatches() == 0 && last.counts.exitUnpaired == 0));
        UNSCOPED_INFO("the drain did not repeat entries");
        CHECK(last.counts.enter == 0);
    }
    // A drain between the return and the reconvergence.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars              cvars(true, false, true);
        std::promise<void> paused, resume;
        const auto         resumed = resume.get_future().share();
        std::thread        other([&]
                                 {
                              (void)runFake(guest, L0, Mode{}, Opts{ .pauseReconverged = [&paused, resumed]
                                                                     {
                                                                         paused.set_value();
                                                                         resumed.wait();
                                                                     } });
                                 });
        paused.get_future().wait();
        UNSCOPED_INFO("a drain after the return keeps the pending gauge");
        CHECK(take().counts.pending == 1);
        UNSCOPED_INFO("the gauge survives a second drain");
        CHECK(take().counts.pending == 1);
        resume.set_value();
        other.join();
        const auto last = take();
        UNSCOPED_INFO("the call paired across two drains");
        CHECK((last.counts.exitOk == 1 && last.counts.pending == 0));
    }
    // Two live calls on different threads keep their own records.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L1);
        guest.install();
        Cvars              cvars(true, false, true);
        std::promise<void> paused, resume;
        const auto         resumed = resume.get_future().share();
        std::thread        other([&]
                                 {
                              (void)runFake(guest, L0, Mode{}, pausing(paused, resumed));
                                 });
        paused.get_future().wait();
        const auto second = runFake(guest, L1);
        resume.set_value();
        other.join();
        const auto counts = take();
        UNSCOPED_INFO("the second thread's call is consumed");
        CHECK(second.replaced);
        UNSCOPED_INFO("both interleaved calls pair");
        CHECK((counts.counts.enter == 2 && counts.counts.consumed == 2 && counts.counts.exitOk == 2));
        UNSCOPED_INFO("no inconclusive record");
        CHECK((counts.counts.exitUnpaired == 0 && counts.mismatches() == 0 && counts.counts.pending == 0));
    }
    // The same SP reused by consecutive calls pairs each.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        (void)runFake(guest, L0);
        (void)runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a reused SP pairs each call");
        CHECK((counts.counts.enter == 2 && counts.counts.exitOk == 2 && counts.counts.exitUnpaired == 0));
    }
    // A call nested below a live frame poisons both and is never exit_ok.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L2);
        guest.install();
        Cvars      cvars(true, false, true);
        Owner      nested;
        const auto outer  = runFake(guest, L0, Mode{}, Opts{ .pauseReturn = [&]
                                                             {
                                                                nested = runFake(guest, L2);
                                                             } });
        const auto counts = take();
        UNSCOPED_INFO("the nested call runs the import once");
        CHECK((outer.replaced && !nested.replaced && nested.setModeCalls.size() == 1));
        UNSCOPED_INFO("the nested call is refused as reentrant and counted as a reentry");
        CHECK((counts.refusal(R::Reentrant) == 1 && counts.refusals() == 1 && counts.counts.reentry == 1));
        UNSCOPED_INFO("a nested pair is never ok");
        CHECK(counts.counts.exitOk == 0);
        UNSCOPED_INFO("three unpaired observations, nothing outstanding");
        CHECK((counts.counts.exitUnpaired == 3 && counts.counts.pending == 0));
    }
    // A nested call counts a reentry even when an earlier refusal wins.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L2);
        guest.install();
        Cvars cvars(true, false, true);
        Owner nested;
        (void)runFake(guest, L0, Mode{}, Opts{ .pauseReturn = [&]
                                               {
                                                   guest.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                                                           {
                                                                               e.graphics = true;
                                                                           });
                                                   nested = runFake(guest, L2);
                                                   guest.mutateEnvironment([](VideoDisplayModeEnvironment& e)
                                                                           {
                                                                               e.graphics = false;
                                                                           });
                                               } });
        const auto counts = take();
        UNSCOPED_INFO("the services refusal wins, the reentry is still reported");
        CHECK((counts.refusal(R::Services) == 1 && counts.refusal(R::Reentrant) == 0 && counts.counts.reentry == 1));
    }
    // An abandoned call is closed as unpaired by the next call on the thread.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars         cvars(true, false, true);
        std::uint64_t r3 = 0x40000000;
        UNSCOPED_INFO("the first call is consumed");
        CHECK((ReplaceVideoDisplayModeCall(makeCall(makeEntryContext(L0, 0x40000000)), r3) && r3 == 0));
        UNSCOPED_INFO("its observations never come");
        CHECK(take().counts.pending == 1);
        const auto owner  = runFake(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("the stale call is unpaired, the new one is ok");
        CHECK((owner.replaced && counts.counts.exitUnpaired == 1 && counts.counts.exitOk == 1 && counts.counts.pending == 0));
    }
    // The outstanding-record bound refuses a call.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L1);
        guest.install();
        Cvars         cvars(true, false, true);
        std::uint64_t r3 = 0x40000000;
        UNSCOPED_INFO("one outstanding call");
        CHECK(ReplaceVideoDisplayModeCall(makeCall(makeEntryContext(L0, 0x40000000)), r3));
        SetVideoDisplayModePendingLimitForTesting(1);
        Owner       other;
        std::thread t([&]
                      {
                          other = runFake(guest, L1);
                      });
        t.join();
        SetVideoDisplayModePendingLimitForTesting(kVideoDisplayModeMaxPending);
        const auto counts = take();
        UNSCOPED_INFO("the bound refuses the new call");
        CHECK((!other.replaced && other.setModeCalls.size() == 1 && counts.refusal(R::Unpaired) == 1));
        finishCall(makeEntryContext(L0, 0));
        const auto closed = take();
        UNSCOPED_INFO("the first call still pairs");
        CHECK((closed.counts.exitOk == 1 && closed.counts.pending == 0));
    }
    // Concurrent entries cannot exceed the outstanding-record bound: every thread passes the
    // bound check, and only then do they race for the few slots.
    {
        constexpr unsigned kThreads = 8, kLimit = 3;
        Guest              guest;
        setup(guest, L0);
        guest.install();
        Cvars                 cvars(true, false, true);
        std::promise<void>    finish;
        const auto            finishReady = finish.get_future().share();
        std::atomic<unsigned> consumed{ 0 }, refused{ 0 }, reached{ 0 };
        std::promise<void>    allDecided;
        probeArrived.store(0);
        probeAll     = {};
        probeReady   = probeAll.get_future().share();
        probeThreads = kThreads;
        SetVideoDisplayModeReserveProbeForTesting(reserveBarrier);
        SetVideoDisplayModePendingLimitForTesting(kLimit);
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < kThreads; ++i)
            threads.emplace_back(
                [&]
                {
                    const auto    entry = makeEntryContext(L0, 0x40000000);
                    std::uint64_t r3    = entry.r3.u64;
                    const bool    taken = ReplaceVideoDisplayModeCall(makeCall(entry), r3);
                    (taken ? consumed : refused).fetch_add(1);
                    if (reached.fetch_add(1) + 1 == kThreads)
                        allDecided.set_value();
                    finishReady.wait();
                    if (taken)
                        finishCall(entry);
                });
        allDecided.get_future().wait();
        SetVideoDisplayModeReserveProbeForTesting(nullptr);
        const auto held = take();
        SetVideoDisplayModePendingLimitForTesting(kVideoDisplayModeMaxPending);
        finish.set_value();
        for (auto& t : threads)
            t.join();
        const auto closed = take();
        UNSCOPED_INFO("exactly the free slots are taken");
        CHECK((consumed.load() == kLimit && refused.load() == kThreads - kLimit));
        UNSCOPED_INFO("the gauge never exceeds the bound");
        CHECK((held.counts.pending == kLimit && held.counts.consumed == kLimit && held.refusal(R::Unpaired) == kThreads - kLimit));
        UNSCOPED_INFO("the admitted calls pair and release their slots");
        CHECK((closed.counts.exitOk == kLimit && closed.counts.pending == 0));
    }
    // Call-free active observations are unpaired.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        finishCall(makeEntryContext(L0, 0));
        const auto counts = take();
        UNSCOPED_INFO("observations without a call are unpaired");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 2 && counts.counts.pending == 0));
    }
    // A duplicated return, a reordered reconvergence and lost observations never count ok.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, false, true);
        const auto entry = makeEntryContext(L0, 0x40000000);
        auto       call  = [&]
        {
            std::uint64_t r3 = entry.r3.u64;
            return ReplaceVideoDisplayModeCall(makeCall(entry), r3);
        };
        const auto returned = [&]
        {
            auto ctx   = entry;
            ctx.r3.u64 = 0;
            ObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);
        };
        const auto reconverged = [&]
        {
            auto ctx   = entry;
            ctx.r3.u64 = ctx.r1.u64 + 80;
            ObserveNativeVideoDisplayModeReconverged(ctx.r1, ctx.r3, ctx.r5, ctx.r31, kNextLr);
        };
        UNSCOPED_INFO("consumed");
        CHECK(call());
        returned();
        returned();
        reconverged();
        auto counts = take();
        UNSCOPED_INFO("a duplicated return poisons the pair");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 2 && counts.counts.pending == 0));

        UNSCOPED_INFO("consumed again");
        CHECK(call());
        reconverged();
        returned();
        counts = take();
        UNSCOPED_INFO("a reconvergence before the return is unpaired");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 2 && counts.counts.pending == 0));

        UNSCOPED_INFO("consumed a third time");
        CHECK(call());
        reconverged();
        counts = take();
        UNSCOPED_INFO("a lost return is unpaired");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.counts.pending == 0));

        UNSCOPED_INFO("consumed a fourth time");
        CHECK(call());
        returned();
        UNSCOPED_INFO("a lost reconvergence stays outstanding");
        CHECK(take().counts.pending == 1);
        UNSCOPED_INFO("the next call closes it");
        CHECK(call());
        counts = take();
        UNSCOPED_INFO("the stale record is unpaired and the new one is outstanding");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 1 && counts.counts.pending == 1));
        returned();
        reconverged();
        counts = take();
        UNSCOPED_INFO("the new call pairs");
        CHECK((counts.counts.exitOk == 1 && counts.counts.pending == 0));
    }
    // Inactive observers with no live record count nothing, whatever r5 holds.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, false);
        finishCall(makeEntryContext(L0, 0));
        const auto counts = take();
        UNSCOPED_INFO("inactive observers with no record count and read nothing");
        CHECK((counts.counts.exitUnpaired == 0 && counts.counts.pending == 0 && guest.reads == 0 && guest.environments == 0));
    }
    // A record outlives a withdrawal and may finish.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars         cvars(true, false, true);
        std::uint64_t r3  = 0x40000000;
        auto          ctx = makeEntryContext(L0, 0x40000000);
        UNSCOPED_INFO("consumed");
        CHECK(ReplaceVideoDisplayModeCall(makeCall(ctx), r3));
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        UNSCOPED_INFO("the slice is inactive after the withdrawal");
        CHECK(!VideoDisplayModeActive());
        finishCall(ctx);
        const auto counts = take();
        UNSCOPED_INFO("the live record finished after the withdrawal");
        CHECK((counts.counts.exitOk == 1 && counts.counts.pending == 0));
    }
}

void testBypass()
{
    for (const int variant : { 0, 1, 2 })
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        // Variant 0: the stub, 1: the exit check alone, 2: both.
        Cvars      cvars(variant != 1, variant != 0, true);
        const auto owner  = runFake(guest, L0, Mode{}, Opts{ .r5 = 0 });
        const auto counts = take();
        UNSCOPED_INFO("the owner bypasses the import");
        CHECK((owner.path == "bypass" && owner.hookCalls == 0 && owner.setModeCalls.empty()));
        UNSCOPED_INFO("the display-information import still runs once");
        CHECK((owner.infoCalls.size() == 1 && owner.infoCalls[0] == L0.sp + 80));
        UNSCOPED_INFO("the bypass leaves every selected-call and quality counter unchanged");
        CHECK((counts.counts.enter == 0 && counts.counts.consumed == 0 && counts.counts.original == 0 && counts.refusals() == 0 && counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 0 &&
               counts.counts.exitUnreadable == 0 && counts.mismatches() == 0 && counts.counts.pending == 0 && counts.counts.reentry == 0));
        UNSCOPED_INFO("the bypass does no memory, admission or log work");
        CHECK((guest.reads == 0 && guest.environments == 0 && guest.logLines().empty()));
    }
    // A bypass neither consumes nor closes an unrelated pending record.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L1);
        guest.install();
        Cvars         cvars(true, false, true);
        std::uint64_t r3  = 0x40000000;
        auto          ctx = makeEntryContext(L0, 0x40000000);
        UNSCOPED_INFO("consumed");
        CHECK(ReplaceVideoDisplayModeCall(makeCall(ctx), r3));
        (void)take();
        // Another thread's bypass has a record of its own and leaves this one alone.
        std::thread other([&]
                          {
                              (void)runFake(guest, L0, Mode{}, Opts{ .r5 = 0 });
                          });
        other.join();
        const auto counts = take();
        UNSCOPED_INFO("the bypass left the pending record alone");
        CHECK((counts.counts.exitUnpaired == 0 && counts.counts.exitOk == 0 && counts.counts.pending == 1));
        finishCall(ctx);
        UNSCOPED_INFO("the record still pairs");
        CHECK(take().counts.exitOk == 1);
        // The same thread: the bypass observers see a live record and a zero r5.
        r3 = 0x40000000;
        UNSCOPED_INFO("consumed again");
        CHECK(ReplaceVideoDisplayModeCall(makeCall(ctx), r3));
        (void)take();
        (void)runFake(guest, L1, Mode{}, Opts{ .r5 = 0 });
        const auto same = take();
        UNSCOPED_INFO("a bypass on the same thread does not close the live record");
        CHECK((same.counts.exitUnpaired == 0 && same.counts.exitOk == 0 && same.counts.pending == 1));
        finishCall(ctx);
        UNSCOPED_INFO("and the record pairs afterwards");
        CHECK(take().counts.exitOk == 1);
    }
}

void testContinuation()
{
    struct Case
    {
        const char* name;
        Mode        mode;
        const char* path;
    };

    const Case cases[] = {
        { "width not 720", Mode{ .width = 1280, .height = 720, .modeWord = 1, .colorWord = 0 }, "direct-width" },
        { "720 by 480 and format 7", Mode{ .width = 720, .height = 480, .modeWord = 7, .colorWord = 1 }, "special" },
        { "720 by 576 and format 54", Mode{ .width = 720, .height = 576, .modeWord = 54, .colorWord = 2 }, "special" },
        { "720 by other height", Mode{ .width = 720, .height = 500, .modeWord = 1, .colorWord = 3 }, "direct-height" },
        { "narrow with format 7", Mode{ .width = 640, .height = 480, .modeWord = 7, .colorWord = 2 }, "direct-width" },
        { "high bits in the input words", Mode{ .width = 1920, .height = 1080, .modeWord = 0xABCDEF07, .colorWord = 0xFFFFFFFD }, "direct-width" },
        { "special with all optional bits", Mode{ .width = 720, .height = 576, .modeWord = 0x36, .colorWord = 3 }, "special" },
    };
    for (const auto& c : cases)
    {
        const std::string name = c.name;
        Guest             sdk, stub;
        setup(sdk, L0, c.mode);
        setup(stub, L0, c.mode);
        Owner a, b;
        {
            Cvars off(false, false, false);
            a = runFake(sdk, L0, c.mode, Opts{ .hooks = false });
        }
        stub.install();
        {
            Cvars on(true, false, true);
            b                 = runFake(stub, L0, c.mode);
            const auto counts = take();
            UNSCOPED_INFO(name + ": the stub run is consumed and observed ok");
            CHECK((counts.counts.consumed == 1 && counts.counts.original == 0 && counts.refusals() == 0 && counts.counts.exitOk == 1 && counts.counts.pending == 0));
        }
        const auto flags = expectedFlags(c.mode);
        UNSCOPED_INFO(name + ": the expected branch of the owner");
        CHECK((a.path == c.path && b.path == c.path));
        UNSCOPED_INFO(name + ": the owner builds the independently derived flags");
        CHECK((a.flagsAtHook == flags && b.flagsAtHook == flags));
        UNSCOPED_INFO(name + ": the replacement hook is reached once on every path");
        CHECK((b.hookCalls == 1 && b.replaced && !a.replaced));
        UNSCOPED_INFO(name + ": the import runs only in the SDK run");
        CHECK((a.setModeCalls.size() == 1 && a.setModeCalls[0] == flags && b.setModeCalls.empty()));
        UNSCOPED_INFO(name + ": the display-information import runs once with SP+80");
        CHECK((a.infoCalls.size() == 1 && b.infoCalls.size() == 1 && a.infoCalls[0] == L0.sp + 80 && b.infoCalls[0] == L0.sp + 80));
        UNSCOPED_INFO(name + ": the full r3 is 0 at 0x826A6264");
        CHECK((a.atReturn.r3.u64 == 0 && b.atReturn.r3.u64 == 0));
        UNSCOPED_INFO(name + ": every register but LR (not captured) is equal at 0x826A6264");
        CHECK(sameContext(a.atReturn, b.atReturn));
        UNSCOPED_INFO(name + ": the whole context, LR included, reconverges after the retained call");
        CHECK(sameContext(a.atReconverged, b.atReconverged));
        UNSCOPED_INFO(name + ": r3 is SP+80 and LR is 0x826A626C at 0x826A626C");
        CHECK((a.atReconverged.r3.u64 == L0.sp + 80 && a.atReconverged.lr == 0x826A626C));
        UNSCOPED_INFO(name + ": the whole context reconverges after the epilogue");
        CHECK(sameContext(a.final, b.final));
        UNSCOPED_INFO(name + ": the guest memory is identical to the SDK-backed run");
        CHECK(sdk.bytes == stub.bytes);
        // An independent oracle for the retained effects and the epilogue.
        UNSCOPED_INFO(name + ": SP, LR and r31 are restored from the saved frame");
        CHECK((b.final.r1.u64 == a.callerSp && b.final.lr == kEntryLr && b.final.r12.u64 == kEntryLr && b.final.r31.u64 == kCallerR31));
        const auto info = modelInfo(1280, 720);
        UNSCOPED_INFO(name + ": the three device stores");
        CHECK((stub.get32(L0.g + 21544) == 1280 && stub.get32(L0.g + 21548) == 720 && stub.get32(L0.g + 21552) == 1280));
        UNSCOPED_INFO(name + ": the neighbouring device words are untouched");
        CHECK((stub.get32(L0.g + 21540) == 0x64000000U + 21540 && stub.get32(L0.g + 21556) == 0x64000000U + 21556));
        bool infoSame = true;
        for (std::uint32_t i = 0; i < 88; i += 4)
        {
            const std::uint32_t want = (std::uint32_t{ info[i] } << 24) | (std::uint32_t{ info[i + 1] } << 16) | (std::uint32_t{ info[i + 2] } << 8) | info[i + 3];
            infoSame                 = infoSame && stub.get32(L0.sp + 80 + i) == want;
        }
        UNSCOPED_INFO(name + ": all 88 display-information bytes are the provider's");
        CHECK(infoSame);
        UNSCOPED_INFO(name + ": the backchain is preserved");
        CHECK(stub.get32(L0.sp) == L0.sp + 192);
    }
    // The bypass still reaches the retained display-information import once.
    {
        Guest sdk, stub;
        setup(sdk, L0);
        setup(stub, L0);
        Owner a, b;
        {
            Cvars off(false, false, false);
            a = runFake(sdk, L0, Mode{}, Opts{ .hooks = false, .r5 = 0 });
        }
        stub.install();
        {
            Cvars on(true, false, true);
            b                 = runFake(stub, L0, Mode{}, Opts{ .r5 = 0 });
            const auto counts = take();
            UNSCOPED_INFO("the bypass run counts nothing");
            CHECK((counts.counts.enter == 0 && counts.counts.exitUnpaired == 0));
        }
        UNSCOPED_INFO("the bypass calls only the display-information import");
        CHECK((a.infoCalls.size() == 1 && b.infoCalls.size() == 1 && a.setModeCalls.empty() && b.setModeCalls.empty()));
        UNSCOPED_INFO("the bypass reconverges identically");
        CHECK((sameContext(a.final, b.final) && sdk.bytes == stub.bytes));
    }
}

void testHookAbi()
{
    static_assert(std::is_same_v<decltype(&ReplaceNativeVideoDisplayModeCall), bool (*)(PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoDisplayModeReturn), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoDisplayModeReconverged), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)>);
    Guest guest;
    setup(guest, L0);
    guest.install();
    Cvars      cvars(true, false, true);
    auto       ctx    = makeEntryContext(L0, 0x40000000 | 0x08000000 | 2);
    const auto before = ctx;
    UNSCOPED_INFO("the hook replaces the call");
    CHECK(ReplaceNativeVideoDisplayModeCall(ctx.r1, ctx.r3, ctx.r31, ctx.lr));
    UNSCOPED_INFO("the full r3 is 0, flags and stale high bits cleared");
    CHECK(ctx.r3.u64 == 0);
    auto expected   = before;
    expected.r3.u64 = 0;
    UNSCOPED_INFO("only r3 changed, whatever the other register classes held");
    CHECK(sameContext(ctx, expected));
    ObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);
    UNSCOPED_INFO("the return observer changes no register");
    CHECK(sameContext(ctx, expected));
    ctx.r3.u64      = ctx.r1.u64 + 80;
    expected.r3.u64 = ctx.r3.u64;
    ObserveNativeVideoDisplayModeReconverged(ctx.r1, ctx.r3, ctx.r5, ctx.r31, kNextLr);
    UNSCOPED_INFO("the reconvergence observer changes no register");
    CHECK(sameContext(ctx, expected));
    const auto counts = take();
    UNSCOPED_INFO("one consumed, ok, none outstanding");
    CHECK((counts.counts.consumed == 1 && counts.counts.exitOk == 1 && counts.counts.pending == 0));
    // A high word in the flags is refused, never masked.
    auto       wide       = makeEntryContext(L0, 0xFFFFFFFF40000000ULL);
    const auto copyOfWide = wide;
    UNSCOPED_INFO("a wide r3 is not the owner's call");
    CHECK((!ReplaceNativeVideoDisplayModeCall(wide.r1, wide.r3, wide.r31, wide.lr) && sameContext(wide, copyOfWide)));
    (void)take();
    finishCall(wide);
    (void)take();
    // A wide r31 is a different device than the validated one.
    auto wideDevice = makeEntryContext(L0, 0x40000000);
    wideDevice.r31.u64 |= 0x1111000000000000ULL;
    const auto copyOfDevice = wideDevice;
    UNSCOPED_INFO("the 32-bit device decides readiness");
    CHECK(ReplaceNativeVideoDisplayModeCall(wideDevice.r1, wideDevice.r3, wideDevice.r31, wideDevice.lr));
    UNSCOPED_INFO("and only r3 changes");
    CHECK((wideDevice.r3.u64 == 0 && sameContext([&]
                                                 {
                                                     auto e   = copyOfDevice;
                                                     e.r3.u64 = 0;
                                                     return e;
                                                 }(),
                                                 wideDevice)));
    (void)take();
    finishCall(wideDevice);
    (void)take();
}

void testReport()
{
    VideoDisplayModeStubCounts counts;
    counts.enter                                       = 7;
    counts.consumed                                    = 5;
    counts.original                                    = 2;
    counts.refusal[static_cast<std::size_t>(R::Shape)] = 2;
    counts.exitOk                                      = 4;
    counts.mismatch[static_cast<std::size_t>(M::Lr)]   = 1;
    counts.reentry                                     = 6;
    counts.pending                                     = 3;
    const auto fields                                  = FormatVideoDisplayModeStubCounts(counts);
    UNSCOPED_INFO("the leading fields");
    CHECK(fields.find(" video_display_mode_enter=7 video_display_mode_consumed=5 video_display_mode_original=2") == 0);
    UNSCOPED_INFO("every refusal has a field");
    CHECK((fields.find(" video_display_mode_refusal_shape=2") != std::string::npos && fields.find(" video_display_mode_refusal_services=0") != std::string::npos));
    UNSCOPED_INFO("refusals in order, then exit_ok");
    CHECK(fields.find(" video_display_mode_refusal_services=0 video_display_mode_refusal_provider=0 video_display_mode_refusal_diagnostics=0 video_display_mode_refusal_stack=0 "
                      "video_display_mode_refusal_shape=2 video_display_mode_refusal_reentrant=0 video_display_mode_refusal_unpaired=0 video_display_mode_exit_ok=4") != std::string::npos);
    UNSCOPED_INFO("every mismatch has a field");
    CHECK(fields.find(" video_display_mode_exit_mismatch_return=0 video_display_mode_exit_mismatch_continuation=0 video_display_mode_exit_mismatch_lr=1") != std::string::npos);
    UNSCOPED_INFO("the tail fields");
    CHECK(fields.find(" video_display_mode_exit_unreadable=0 video_display_mode_exit_unpaired=0 video_display_mode_reentry=6 video_display_mode_pending=3") != std::string::npos);
    UNSCOPED_INFO("a periodic line has no final markers");
    CHECK(fields.find("final") == std::string::npos);
    counts.final = true;
    UNSCOPED_INFO("an outstanding call makes the final incomplete");
    CHECK(FormatVideoDisplayModeStubCounts(counts).find(" video_display_mode_final_incomplete=1 video_display_mode_final_skipped=0") != std::string::npos);
    counts.pending = 0;
    UNSCOPED_INFO("a clean final");
    CHECK(FormatVideoDisplayModeStubCounts(counts).find(" video_display_mode_final_incomplete=0 video_display_mode_final_skipped=0") != std::string::npos);

    static std::vector<std::string> logs;
    logs.clear();
    const VideoDisplayModeReportOperations report{ FormatVideoDisplayModeStubCounts, [](std::string_view message)
                                                   {
                                                       logs.emplace_back(message);
                                                   } };
    Cvars cvars(true, false, true);
    // Counts drain per window; the pending gauge stays.
    Guest guest;
    setup(guest, L0);
    guest.install();
    const auto owner  = runFake(guest, L0);
    const auto window = ReportVideoDisplayModeStub(false, report);
    UNSCOPED_INFO("the window reports the call");
    CHECK((window.find(" video_display_mode_enter=1 video_display_mode_consumed=1") == 0 && window.find("video_display_mode_exit_ok=1") != std::string::npos));
    UNSCOPED_INFO("the next window starts at zero");
    CHECK(ReportVideoDisplayModeStub(false, report).find(" video_display_mode_enter=0 video_display_mode_consumed=0") == 0);
    UNSCOPED_INFO("a periodic drain does not log");
    CHECK(logs.empty());
    const auto final = ReportVideoDisplayModeStub(true, report);
    UNSCOPED_INFO("the shutdown final is logged");
    CHECK((!logs.empty() && logs.back() == final && final.find("video_display_mode_final_incomplete=0") != std::string::npos));
    (void)owner;
    // A concurrent drain holding the lock does not stall shutdown, and the skip is retained.
    {
        logs.clear();
        auto& mutex = detail::VideoDisplayModeReportMutex();
        mutex.lock();
        const auto  begun = std::chrono::steady_clock::now();
        std::string skipped;
        std::thread t([&]
                      {
                          skipped = ReportVideoDisplayModeStub(true, report);
                      });
        t.join();
        const auto elapsed = std::chrono::steady_clock::now() - begun;
        mutex.unlock();
        UNSCOPED_INFO("the final drain is bounded");
        CHECK(elapsed < std::chrono::seconds(2));
        UNSCOPED_INFO("the skipped final says so");
        CHECK(skipped.find("video_display_mode_final_incomplete=1 video_display_mode_final_skipped=1") != std::string::npos);
        UNSCOPED_INFO("the skipped final is logged");
        CHECK((!logs.empty() && logs.back() == skipped));
        const auto next = ReportVideoDisplayModeStub(true, report);
        UNSCOPED_INFO("the skip is reported by the next final");
        CHECK(next.find("video_display_mode_final_skipped=1") != std::string::npos);
        UNSCOPED_INFO("and only once");
        CHECK(ReportVideoDisplayModeStub(true, report).find("video_display_mode_final_skipped=0") != std::string::npos);
    }
    // An outstanding call at shutdown is an incomplete final and the gauge survives it.
    {
        std::uint64_t r3  = 0x40000000;
        auto          ctx = makeEntryContext(L0, 0x40000000);
        UNSCOPED_INFO("consumed");
        CHECK(ReplaceVideoDisplayModeCall(makeCall(ctx), r3));
        const auto last = ReportVideoDisplayModeStub(true, report);
        UNSCOPED_INFO("an unfinished record makes the final incomplete");
        CHECK((last.find("video_display_mode_pending=1") != std::string::npos && last.find("video_display_mode_final_incomplete=1") != std::string::npos));
        UNSCOPED_INFO("the gauge survives the final drain");
        CHECK(take().counts.pending == 1);
        finishCall(ctx);
        (void)take();
    }
    // A throwing report logger is contained.
    {
        const VideoDisplayModeReportOperations throwing{ FormatVideoDisplayModeStubCounts, [](std::string_view)
                                                         {
                                                             throw std::runtime_error("injected report fault");
                                                         } };
        UNSCOPED_INFO("a throwing logger still returns the fields");
        CHECK(!ReportVideoDisplayModeStub(true, throwing).empty());
    }
}

// The first sixteen ok lines are logged, every other record always, with every named field.
void testLogging()
{
    Guest guest;
    setup(guest, L0);
    guest.install();
    Cvars cvars(true, false, true);
    for (int i = 0; i < 20; ++i)
        (void)runFake(guest, L0);
    auto logs = guest.logLines();
    UNSCOPED_INFO("only the first sixteen ok records are logged");
    CHECK(logs.size() == 16);
    (void)runFake(guest, L0, Mode{}, Opts{}, Model{ .infoFault = 2 });
    logs = guest.logLines();
    UNSCOPED_INFO("a bad record is always logged");
    CHECK((logs.size() == 17 && logs.back().find("verdict=mismatch") != std::string::npos));
    const char* fieldNames[] = { "pc=", "thread=", "invocation=", "sp=", "sp_full=", "g=", "g_full=", "flags_full=", "entry_lr=", "replaced=", "result_valid=", "result_full=", "return_lr_valid=0", "continuation_r3=", "reconverged_lr=", "runtime_id=", "service_id=", "ring_service_id=", "provider_target=", "provider_valid=1", "display_info_valid=1", "display_info_be_hex=", "display_width=1280", "display_height=720", "actual_display_width=1280", "return=ok", "continuation=ok", "lr=ok", "provider=ok", "unreadable=0", "verdict=ok" };
    for (const auto* field : fieldNames)
    {
        UNSCOPED_INFO(std::string("the log carries ") + field);
        CHECK(logs[0].find(std::string(" ") + field) != std::string::npos);
    }
    UNSCOPED_INFO("the raw flags, entry LR and replacement are recorded");
    CHECK((logs[0].find(" replaced=1 ") != std::string::npos && logs[0].find(" entry_lr=00000000826A4900 ") != std::string::npos && logs[0].find(" flags_full=0000000040000000 ") != std::string::npos));
    const auto expectedHex = [&]
    {
        std::string hex;
        for (const auto byte : modelInfo(1280, 720))
            hex += std::format("{:02X}", byte);
        return hex;
    }();
    UNSCOPED_INFO("all 88 bytes are logged in order");
    CHECK(logs[0].find("display_info_be_hex=" + expectedHex + " ") != std::string::npos);
    // A throwing logger never changes the counts.
    {
        guest.throwOnLog = true;
        (void)take();
        (void)runFake(guest, L0, Mode{}, Opts{}, Model{ .infoFault = 2 });
        const auto counts = take();
        UNSCOPED_INFO("a throwing logger is contained");
        CHECK((counts.counts.exitOk == 0 && counts.mismatch(M::Lr) == 1 && counts.counts.pending == 0));
        guest.throwOnLog = false;
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

    void restore(std::uint32_t address, const std::vector<std::uint8_t>& bytes) const
    {
        std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), bytes.data(), bytes.size());
    }
};

std::string infoHexOf(const std::string& line)
{
    const std::string key = "display_info_be_hex=";
    const auto        at  = line.find(key);
    if (at == std::string::npos)
        return {};
    const auto begin = at + key.size();
    return line.substr(begin, line.find(' ', begin) - begin);
}

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

    // The actual VdSetDisplayMode export: every valid flag word and a poisoned full 64-bit word.
    {
        const std::uint32_t scratch = memory->SystemHeapAlloc(0x1000, 0x1000, rex::memory::kSystemHeapVirtual);
        UNSCOPED_INFO("scratch allocation");
        CHECK(scratch != 0);
        if (scratch)
        {
            for (std::uint32_t offset = 0; offset < 0x1000; offset += 4)
                real.put32(scratch + offset, 0x5A000000 | offset);
            const auto                 image      = real.snapshot(scratch, 0x1000);
            const std::uint32_t        optional[] = { 0x08000000, 0x10000000, 0x20000000, 0x2 };
            std::vector<std::uint64_t> inputs;
            for (unsigned combo = 0; combo < 16; ++combo)
            {
                std::uint32_t flags = 0x40000000;
                for (unsigned bit = 0; bit < 4; ++bit)
                    if (combo & (1U << bit))
                        flags |= optional[bit];
                inputs.push_back(flags);
            }
            inputs.push_back(0xFEED000040000000ULL);
            inputs.push_back(0xFFFFFFFFFFFFFFFFULL);
            for (const auto flags : inputs)
            {
                PPCContext sdk{};
                std::memset(&sdk, 0x35, sizeof(sdk));
                sdk.r3.u64        = flags;
                sdk.lr            = kEntryLr;
                const auto before = sdk;
                __imp__VdSetDisplayMode(sdk, base);
                auto expected   = before;
                expected.r3.u64 = 0;
                UNSCOPED_INFO("the SDK export returns 0 in the full r3");
                CHECK(sdk.r3.u64 == 0);
                UNSCOPED_INFO("the SDK export changes no other register");
                CHECK(sameContext(sdk, expected));
                UNSCOPED_INFO("the SDK export writes no guest byte");
                CHECK(real.snapshot(scratch, 0x1000) == image);
            }
        }
    }

    // The owner over the real memory and the actual next SDK export: a plain SDK run, the exit-check
    // baseline and the stub candidate.
    const std::uint32_t frame  = memory->SystemHeapAlloc(0x3000, 0x1000, rex::memory::kSystemHeapVirtual);
    const std::uint32_t device = memory->SystemHeapAlloc(0x6000, 0x1000, rex::memory::kSystemHeapVirtual);
    UNSCOPED_INFO("owner allocations");
    CHECK((frame && device));
    if (!(frame && device))
        return;
    const Layout layout{ frame + 0x1000, device };
    const Mode   mode{ .width = 720, .height = 480, .modeWord = 7, .colorWord = 1 };
    const auto   fill = [&]
    {
        for (std::uint32_t offset = 0; offset < 0x3000; offset += 4)
            real.put32(frame + offset, 0x5A000000 | offset);
        for (std::uint32_t offset = 0; offset < 0x6000; offset += 4)
            real.put32(device + offset, 0x64000000 | offset);
        real.put32(layout.sp, layout.sp + 192);
        for (std::uint32_t offset = 80; offset < 80 + 88; offset += 4)
            real.put32(layout.sp + offset, kPoison);
        real.put32(layout.sp + kInput + 0, mode.width);
        real.put32(layout.sp + kInput + 4, mode.height);
        real.put32(layout.sp + kInput + 64, mode.modeWord);
        real.put32(layout.sp + kInput + 68, mode.colorWord);
    };
    unsigned infoFault = 0;
    Backend  io;
    io.get32 = [&real](std::uint32_t address)
    {
        return real.get32(address);
    };
    io.put32 = [&real](std::uint32_t address, std::uint32_t value)
    {
        real.put32(address, value);
    };
    io.setMode = [base](PPCContext& ctx)
    {
        __imp__VdSetDisplayMode(ctx, base);
    };
    io.getInfo = [&](PPCContext& ctx)
    {
        __imp__VdGetCurrentDisplayInformation(ctx, base);
        if (infoFault == 1)
            real.put32(ctx.r3.u32 + 40, real.get32(ctx.r3.u32 + 40) ^ 0x00010000);
    };

    VideoDisplayModeOperations ops;
    ops.memory.read32 = [&real, memory](std::uint32_t address, std::uint32_t& value)
    {
        if (address < 0x10000 || !memory->LookupHeap(address))
            return false;
        value = real.get32(address);
        return true;
    };
    ops.environment = []
    {
        return goodEnvironment();
    };
    std::vector<std::string> logs;
    ops.log = [&logs](std::string_view message)
    {
        logs.emplace_back(message);
    };

    fill();
    const auto frameImage  = real.snapshot(frame, 0x3000);
    const auto deviceImage = real.snapshot(device, 0x6000);
    Owner      plain;
    {
        Cvars off(false, false, false);
        plain = runOwner(io, layout, mode, Opts{ .hooks = false });
    }
    const auto plainFrame  = real.snapshot(frame, 0x3000);
    const auto plainDevice = real.snapshot(device, 0x6000);
    UNSCOPED_INFO("the plain run calls both exports once");
    CHECK((plain.setModeCalls.size() == 1 && plain.setModeCalls[0] == expectedFlags(mode) && plain.infoCalls.size() == 1));

    SetVideoDisplayModeOperationsForTesting(&ops);
    // The exit-check baseline.
    real.restore(frame, frameImage);
    real.restore(device, deviceImage);
    Owner baseline;
    {
        Cvars check(false, true, true, layout.g);
        baseline          = runOwner(io, layout, mode);
        const auto counts = take();
        UNSCOPED_INFO("the baseline pairs ok");
        CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.counts.exitOk == 1 && counts.refusals() == 0 && counts.mismatches() == 0));
    }
    const auto baselineHex = logs.empty() ? std::string{} : infoHexOf(logs.back());
    UNSCOPED_INFO("the baseline logs the 88 bytes");
    CHECK(baselineHex.size() == 176);
    // The stub candidate.
    logs.clear();
    real.restore(frame, frameImage);
    real.restore(device, deviceImage);
    Owner candidate;
    {
        Cvars stub(true, false, true, layout.g);
        candidate         = runOwner(io, layout, mode);
        const auto counts = take();
        UNSCOPED_INFO("the candidate is consumed and pairs ok");
        CHECK((counts.counts.enter == 1 && counts.counts.consumed == 1 && counts.counts.exitOk == 1 && counts.refusals() == 0 && counts.mismatches() == 0));
    }
    const auto candidateHex = logs.empty() ? std::string{} : infoHexOf(logs.back());
    UNSCOPED_INFO("the candidate's display information equals the baseline's byte for byte");
    CHECK((!candidateHex.empty() && candidateHex == baselineHex));
    UNSCOPED_INFO("the whole guest memory equals the plain SDK run");
    CHECK((real.snapshot(frame, 0x3000) == plainFrame && real.snapshot(device, 0x6000) == plainDevice));
    UNSCOPED_INFO("the final contexts are equal");
    CHECK((sameContext(plain.final, candidate.final) && sameContext(plain.final, baseline.final)));
    UNSCOPED_INFO("the contexts reconverge after the actual display-information export");
    CHECK(sameContext(plain.atReconverged, candidate.atReconverged));
    UNSCOPED_INFO("the stub skips only SetDisplayMode");
    CHECK((candidate.setModeCalls.empty() && baseline.setModeCalls.size() == 1 && candidate.infoCalls.size() == 1));
    // The device stores are the three BE16 values of the actual information.
    UNSCOPED_INFO("the device stores are the display width, height and actual width");
    CHECK((real.get32(device + 21544) == (real.get32(layout.sp + 152) >> 16) && real.get32(device + 21548) == (real.get32(layout.sp + 152) & 0xFFFF) &&
           real.get32(device + 21552) == (real.get32(layout.sp + 164) & 0xFFFF)));
    // A corrupted middle byte of the information is a different byte string, whatever the width says.
    logs.clear();
    infoFault = 1;
    real.restore(frame, frameImage);
    real.restore(device, deviceImage);
    {
        Cvars stub(true, false, true, layout.g);
        (void)runOwner(io, layout, mode);
        (void)take();
    }
    infoFault             = 0;
    const auto corruptHex = logs.empty() ? std::string{} : infoHexOf(logs.back());
    UNSCOPED_INFO("a corrupted middle byte is visible in the logged bytes");
    CHECK((corruptHex.size() == 176 && corruptHex != candidateHex));
    UNSCOPED_INFO("only the middle byte differs, so width-only parity would miss it");
    CHECK((corruptHex.substr(0, 80) == candidateHex.substr(0, 80) && corruptHex.substr(144) == candidateHex.substr(144)));
    SetVideoDisplayModeOperationsForTesting(nullptr);
}

void WrongDisplayModeTarget(PPCContext&, uint8_t*)
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
    SetVideoDisplayModeOperationsForTesting(nullptr);
    const std::uint32_t frame  = memory->SystemHeapAlloc(0x3000, 0x1000, rex::memory::kSystemHeapVirtual);
    const std::uint32_t device = memory->SystemHeapAlloc(0x6000, 0x1000, rex::memory::kSystemHeapVirtual);
    UNSCOPED_INFO("allocations");
    CHECK((frame && device));
    if (!(frame && device))
        return;
    const Layout layout{ frame + 0x1000, device };
    for (std::uint32_t offset = 0; offset < 0x3000; offset += 4)
        real.put32(frame + offset, 0x5A000000 | offset);
    real.put32(layout.sp, layout.sp + 192);
    Cvars cvars(true, false, true, layout.g);

    // The environment as the production probe reads it.
    auto environment = ProductionVideoDisplayModeEnvironmentForTesting();
    UNSCOPED_INFO("the Runtime is found");
    CHECK((environment.runtime && environment.identity.runtime != 0));
    UNSCOPED_INFO("no graphics system and no detached delegate");
    CHECK((!environment.graphics && !environment.delegate && environment.identity.delegate == 0));
    UNSCOPED_INFO("no function table: the target is absent");
    CHECK((environment.dispatcher && !environment.targetPresent && !environment.targetMatches && environment.identity.target == 0));
    auto* dispatcher = runtime.function_dispatcher();
    UNSCOPED_INFO("dispatcher");
    CHECK(dispatcher != nullptr);
    if (!dispatcher)
        return;
    UNSCOPED_INFO("a function table over the title image");
    CHECK(dispatcher->InitializeFunctionTable(0x82000000, 0x00F00000, 0x82000000, 0x00F00000));
    UNSCOPED_INFO("a different function at the import");
    CHECK(dispatcher->SetFunction(kVideoDisplayModeImportAddress, &WrongDisplayModeTarget));
    environment = ProductionVideoDisplayModeEnvironmentForTesting();
    UNSCOPED_INFO("a different target is present and does not match");
    CHECK((environment.targetPresent && !environment.targetMatches && environment.identity.target == reinterpret_cast<std::uintptr_t>(&WrongDisplayModeTarget)));
    UNSCOPED_INFO("the retained wrapper at the import");
    CHECK(dispatcher->SetFunction(kVideoDisplayModeImportAddress, &__imp__VdSetDisplayMode));
    environment = ProductionVideoDisplayModeEnvironmentForTesting();
    UNSCOPED_INFO("the retained wrapper matches");
    CHECK((environment.targetPresent && environment.targetMatches && environment.identity.target == reinterpret_cast<std::uintptr_t>(&__imp__VdSetDisplayMode)));
    UNSCOPED_INFO("the ring services identity is bound");
    CHECK(environment.identity.ring == reinterpret_cast<std::uintptr_t>(rerevved::gpu::guest_d3d::CurrentRingStubServices().get()));

    // A call over the production adapters: no delegate, so the services refusal runs the import.
    const auto    entry   = makeEntryContext(layout, 0x40000000);
    std::uint64_t r3      = entry.r3.u64;
    const bool    taken   = ReplaceVideoDisplayModeCall(makeCall(entry), r3);
    const auto    refused = take();
    UNSCOPED_INFO("without a delegate the call is a services refusal");
    CHECK((!taken && r3 == entry.r3.u64 && refused.refusal(R::Services) == 1 && refused.refusals() == 1 && refused.counts.original == 1));
    finishCall(entry);
    (void)take();
    // A withdrawal ends admission.
    rerevved::gpu::guest_d3d::ringStubsActive.store(false);
    r3 = entry.r3.u64;
    UNSCOPED_INFO("an unpublished route is inactive");
    CHECK((!ReplaceVideoDisplayModeCall(makeCall(entry), r3) && take().counts.enter == 0));
    // The production memory read is a checked big-endian load of the runtime's guest memory.
    std::uint32_t word = 0;
    UNSCOPED_INFO("the backchain is read from the runtime's guest memory");
    CHECK((ProductionVideoDisplayModeRead32ForTesting(layout.sp, word) && word == layout.sp + 192));
    UNSCOPED_INFO("an unmapped or wrapping address is unreadable");
    CHECK((!ProductionVideoDisplayModeRead32ForTesting(0x1000, word) && !ProductionVideoDisplayModeRead32ForTesting(0xFFFFFFFE, word)));
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 17>;

const CaseTable kCases{
    CaseTable::value_type{ "testConstants", testConstants },
    CaseTable::value_type{ "testDeriveAndApply", testDeriveAndApply },
    CaseTable::value_type{ "testFlagShapes", testFlagShapes },
    CaseTable::value_type{ "testRefusalTable", testRefusalTable },
    CaseTable::value_type{ "testOriginalRunsOnce", testOriginalRunsOnce },
    CaseTable::value_type{ "testServicesAndLease", testServicesAndLease },
    CaseTable::value_type{ "testDisabledWork", testDisabledWork },
    CaseTable::value_type{ "testExitCheck", testExitCheck },
    CaseTable::value_type{ "testIdentityChanges", testIdentityChanges },
    CaseTable::value_type{ "testObservationPairing", testObservationPairing },
    CaseTable::value_type{ "testBypass", testBypass },
    CaseTable::value_type{ "testContinuation", testContinuation },
    CaseTable::value_type{ "testHookAbi", testHookAbi },
    CaseTable::value_type{ "testReport", testReport },
    CaseTable::value_type{ "testLogging", testLogging },
    CaseTable::value_type{ "testRuntimeParity", testRuntimeParity },
    CaseTable::value_type{ "testRuntimeAdmission", testRuntimeAdmission }
};

// Runs every table entry, or only the entry named `selected`.
unsigned runCases(std::string_view selected)
{
    unsigned ran = 0;
    for (const auto& [name, run] : kCases)
        if (selected.empty() || selected == name)
        {
            INFO("case " << name);
            run();
            ++ran;
        }
    return ran;
}

} // namespace

TEST_CASE("video_display_mode_stub", "[ctest][video-display-mode-stub][timeout-120]")
{
    runCases({});
}

// Runs one named case:
//   video-display-mode-stub.case <testName>
TEST_CASE("video_display_mode_stub_selected", "[.][manual][video-display-mode-stub]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(videoDisplayModeStubCase, "video-display-mode-stub.case", "video_display_mode_stub_selected")
