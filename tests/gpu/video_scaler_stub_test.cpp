#include "allocation_harness.hpp"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/video_scaler_stub.h"
#include "manual_cases.hpp"

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

REXCVAR_DECLARE(bool, native_video_scaler_stub);
REXCVAR_DECLARE(bool, native_video_scaler_exit_check);

// The hook adapters the generated code calls (video_scaler_hooks.cpp).
bool ReplaceNativeVideoScalerCall(PPCRegister& r1,
                                  PPCRegister& r3,
                                  PPCRegister& r4,
                                  PPCRegister& r5,
                                  PPCRegister& r6,
                                  PPCRegister& r7,
                                  PPCRegister& r8,
                                  PPCRegister& r9,
                                  PPCRegister& r10,
                                  PPCRegister& r30,
                                  PPCRegister& r31);
void ObserveNativeVideoScalerReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r30, PPCRegister& r31);
void ObserveNativeVideoScalerCopy(PPCRegister& r1, PPCRegister& r30, PPCRegister& r31);

// The SDK export the owner calls when the import runs.
extern "C" void __imp__VdInitializeScalerCommandBuffer(PPCContext& ctx, uint8_t* base);

namespace
{

using namespace rerevved::gpu::guest_d3d;
using rex::X_STATUS;
using R = VideoScalerRefusal;
using M = VideoScalerMismatch;

constexpr std::uint32_t kBelow = 0x51B0E1ED, kAbove = 0xA5E4714E, kHeader = 0x68656164, kTail = 0x7461696C, kOldCursor = 0x0BADC0DE;
constexpr std::uint32_t kOutputPoison = 0xDEADBEEF, kCopyPoison = 0xCCCCCCCC;
constexpr std::uint32_t kCallerLr = 0x826B3000, kPreCallLr = 0x826B0414;

// One owner invocation's addresses: its stack pointer, the device G and the
// ring reservation R.
struct Layout
{
    std::uint32_t sp, g, r;
};

constexpr Layout L0{ 0x80000, 0x20000, 0xE0010010 };
constexpr Layout L1{ 0x90000, 0x20000, 0xE0020010 };
constexpr Layout L2{ 0x70000, 0x20000, 0xE0030010 }; // below L0 on the stack

struct Region
{
    std::uint32_t lo, size;
    std::uint64_t identity;
    bool          read = true, write = true;
};

// A guest address space with checked access and the identity the production
// adapter derives: a virtual region is 2^32 + address, a physical one its
// physical address.
class Guest
{
public:
    std::mutex                                           mutex;
    std::map<std::uint32_t, std::uint8_t>                bytes;
    std::vector<Region>                                  regions;
    std::set<std::uint32_t>                              unreadable;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stores;          // successful stub stores
    std::size_t                                          failAt      = 0; // 1-based stub store that fails
    bool                                                 throwAtFail = false, throwOnLog = false;
    std::function<void(std::size_t)>                     onStore;
    std::atomic<unsigned>                                reads{ 0 }, identities{ 0 }, writeAttempts{ 0 };
    std::vector<std::string>                             terminals, logs;
    VideoScalerOperations                                ops;
    bool                                                 installed = false;

    Guest()                        = default;
    Guest(const Guest&)            = delete;
    Guest& operator=(const Guest&) = delete;

    ~Guest()
    {
        if (installed)
            SetVideoScalerOperationsForTesting(nullptr);
    }

    const Region* find(std::uint32_t address, std::uint32_t size) const
    {
        if (size == 0 || address > 0xFFFFFFFFU - (size - 1))
            return nullptr;
        for (const auto& region : regions)
            if (address >= region.lo && std::uint64_t{ address } + size <= std::uint64_t{ region.lo } + region.size)
                return &region;
        return nullptr;
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

    // The owner's own accesses, not the stub's.
    std::uint32_t get32(std::uint32_t address)
    {
        std::lock_guard lock(mutex);
        return rawGet32(address);
    }

    std::uint8_t get8(std::uint32_t address)
    {
        std::lock_guard lock(mutex);
        return rawGet8(address);
    }

    void put32(std::uint32_t address, std::uint32_t value)
    {
        std::lock_guard lock(mutex);
        rawPut32(address, value);
    }

    void put8(std::uint32_t address, std::uint8_t value)
    {
        std::lock_guard lock(mutex);
        bytes[address] = value;
    }

    void add(Region region)
    {
        std::lock_guard lock(mutex);
        regions.push_back(region);
    }

    VideoScalerMemory memory()
    {
        VideoScalerMemory memory;
        memory.read32 = [this](std::uint32_t address, std::uint32_t& value)
        {
            reads.fetch_add(1);
            std::lock_guard lock(mutex);
            const auto*     region = find(address, 4);
            if (!region || !region->read || unreadable.count(address))
                return false;
            value = rawGet32(address);
            return true;
        };
        memory.identity = [this](std::uint32_t address, std::uint32_t size, bool write) -> std::optional<std::uint64_t>
        {
            identities.fetch_add(1);
            std::lock_guard lock(mutex);
            const auto*     region = find(address, size);
            if (!region || !region->read || (write && !region->write))
                return std::nullopt;
            return region->identity + (address - region->lo);
        };
        memory.write32 = [this](std::uint32_t address, std::uint32_t value)
        {
            const auto index = writeAttempts.fetch_add(1) + 1;
            if (onStore)
                onStore(index);
            std::lock_guard lock(mutex);
            if (failAt && index == failAt)
            {
                if (throwAtFail)
                    throw std::runtime_error("injected store fault");
                return false;
            }
            const auto* region = find(address, 4);
            if (!region || !region->write)
                return false;
            stores.emplace_back(address, value);
            rawPut32(address, value);
            return true;
        };
        return memory;
    }

    // Installs this address space as the stub's memory for the test.
    void install()
    {
        ops = VideoScalerOperations{ memory(),
                                     [this](std::string_view reason)
                                     {
                                         std::lock_guard lock(mutex);
                                         terminals.emplace_back(reason);
                                     },
                                     [this](std::string_view message)
                                     {
                                         std::lock_guard lock(mutex);
                                         if (throwOnLog)
                                             throw std::runtime_error("injected log fault");
                                         logs.emplace_back(message);
                                     } };
        installed = true;
        SetVideoScalerOperationsForTesting(&ops);
    }
};

void setup(Guest& guest, const Layout& l)
{
    guest.add({ l.sp - 0x1000, 0x3000, (std::uint64_t{ 1 } << 32) + l.sp - 0x1000 });
    if (!guest.find(l.g, 0x6000))
        guest.add({ l.g, 0x6000, (std::uint64_t{ 1 } << 32) + l.g });
    guest.add({ l.r - 0x10, 0x2000, std::uint64_t{ l.r } - 0xE0000000ULL - 0x10 });
    for (std::uint32_t offset = 0; offset < 2464; offset += 4)
        guest.put32(l.sp + offset, 0x5A000000 | offset);
    for (std::uint32_t i = 0; i < 200; ++i)
        guest.put32(l.sp + 192 + 4 * i, kOutputPoison);
    guest.put32(l.sp + 188, kBelow);
    guest.put32(l.sp + 992, kAbove);
    // The stack arguments sit in the low words of 8-byte slots; the high words are poison.
    for (std::uint32_t slot = 0; slot < 5; ++slot)
    {
        guest.put32(l.sp + 80 + 8 * slot, 0xBAD00000 | slot);
        guest.put32(l.sp + 84 + 8 * slot, 0xBAD1000 | slot);
    }
    guest.put32(l.sp + 84, l.sp + 172);
    guest.put32(l.sp + 92, l.sp + 992);
    guest.put32(l.sp + 100, l.sp + 192);
    guest.put32(l.sp + 108, 200);
    guest.put32(l.sp + 116, 0xBAD1116);
    // The filter parameter and coefficient blocks the import must not touch.
    for (std::uint32_t i = 0; i < 4; ++i)
        guest.put32(l.sp + 156 + 4 * i, 0xF1000000 | i);
    for (std::uint32_t i = 0; i < 4; ++i)
        guest.put32(l.sp + 172 + 4 * i, 0xC0000000 | i);
    for (std::uint32_t offset = 0; offset < 0x40; offset += 4)
        guest.put32(l.g + offset, 0x64000000 | offset);
    guest.put32(l.g + 0x30, kOldCursor);
    for (std::uint32_t offset = 0; offset < 0x30; offset += 4)
        guest.put32(l.r - 0x10 + offset, 0x72000000 | offset);
    guest.put32(l.r, kHeader);
    for (std::uint32_t i = 0; i < 200; ++i)
        guest.put32(l.r + 4 + 4 * i, kCopyPoison);
    guest.put32(l.r + 804, kTail);
}

struct Cvars
{
    Cvars(bool stub, bool check, bool published, std::uint32_t validated = L0.g)
    {
        REXCVAR_SET(native_video_scaler_stub, stub);
        REXCVAR_SET(native_video_scaler_exit_check, check);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(published ? validated : 0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(published);
        (void)TakeVideoScalerStubCounts();
    }

    ~Cvars()
    {
        REXCVAR_SET(native_video_scaler_stub, false);
        REXCVAR_SET(native_video_scaler_exit_check, false);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        SetVideoScalerPendingLimitForTesting(kVideoScalerMaxPending);
    }
};

PPCContext makeContext(const Layout& l)
{
    PPCContext ctx{};
    std::memset(&ctx, 0x35, sizeof(ctx));
    ctx.r1.u64  = l.sp;
    ctx.r25.u64 = 0xFEED000000200010ULL; // source xy with stale high bits
    ctx.r3.u64  = ctx.r25.u64;           // mr r3,r25
    ctx.r4.u64  = 0x0000000002D00500ULL;
    ctx.r5.u64  = 0x0000000000100008ULL;
    ctx.r6.u64  = 0x0000000002D00500ULL;
    ctx.r7.u64  = 0x00000000040002D0ULL;
    ctx.r8.u64  = 0x0000000000000007ULL;
    ctx.r9.u64  = 0x7777000000000000ULL | (l.sp + 156);
    ctx.r10.u64 = 0x0000000000000007ULL;
    ctx.r30.u64 = l.r;
    ctx.r31.u64 = l.g;
    ctx.lr      = kCallerLr;
    return ctx;
}

VideoScalerCall makeCall(const PPCContext& ctx)
{
    VideoScalerCall call;
    call.r1           = ctx.r1.u64;
    call.arguments[0] = ctx.r3.u64;
    call.arguments[1] = ctx.r4.u64;
    call.arguments[2] = ctx.r5.u64;
    call.arguments[3] = ctx.r6.u64;
    call.arguments[4] = ctx.r7.u64;
    call.arguments[5] = ctx.r8.u64;
    call.arguments[6] = ctx.r9.u64;
    call.arguments[7] = ctx.r10.u64;
    call.r30          = ctx.r30.u64;
    call.r31          = ctx.r31.u64;
    return call;
}

// Every register; the struct's tail padding is not guest state and is not copied faithfully.
bool sameContext(const PPCContext& a, const PPCContext& b)
{
    constexpr std::size_t kState = offsetof(PPCContext, v127) + sizeof(PPCVRegister);
    return std::memcmp(&a, &b, kState) == 0;
}

// The inputs of one import call as the SDK export sees them.
struct ImportCall
{
    std::uint64_t reg[8]{};
    std::uint32_t slot[4]{};
    std::uint32_t sp = 0, g = 0, r = 0;

    bool operator==(const ImportCall& other) const
    {
        return std::memcmp(reg, other.reg, sizeof(reg)) == 0 && std::memcmp(slot, other.slot, sizeof(slot)) == 0 && sp == other.sp && g == other.g && r == other.r;
    }
};

struct Opts
{
    bool                                     hooks       = true;
    unsigned                                 sdkFault    = 0; // 1: count 199, 2: one wrong word, 3: clobber [SP+188]
    unsigned                                 copyFault   = 0; // 1: one wrong copied word
    unsigned                                 cursorFault = 0; // 1: cursor R+796
    std::function<void(Guest&, PPCContext&)> tweak;           // after the argument setup
    std::function<void()>                    pause;           // at 0x826B0434, before the return observation
};

struct Owner
{
    PPCContext                                           atHook{}, afterHook{}, atReturn{}, final{};
    std::vector<std::string>                             trace;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> c7a8;
    std::vector<ImportCall>                              imports;
    ImportCall                                           beforeHook;
    unsigned                                             lrReadsBeforeCopy = 0;
    std::uint64_t                                        r29AtCopy         = 0;
    bool                                                 replaced          = false;
    std::uint32_t                                        callerSp          = 0;
};

// The caller's nonvolatile r25..r31 at entry, saved by the owner's prologue (__savegprlr_25).
constexpr std::uint64_t callerRegister(unsigned k)
{
    return 0xCA11000000002500ULL + k;
}

ImportCall importCall(Guest& guest, const PPCContext& ctx)
{
    ImportCall call;
    call.reg[0] = ctx.r3.u64;
    call.reg[1] = ctx.r4.u64;
    call.reg[2] = ctx.r5.u64;
    call.reg[3] = ctx.r6.u64;
    call.reg[4] = ctx.r7.u64;
    call.reg[5] = ctx.r8.u64;
    call.reg[6] = ctx.r9.u64;
    call.reg[7] = ctx.r10.u64;
    for (std::uint32_t i = 0; i < 4; ++i)
        call.slot[i] = guest.get32(ctx.r1.u32 + 84 + 8 * i);
    call.sp = ctx.r1.u32;
    call.g  = ctx.r31.u32;
    call.r  = ctx.r30.u32;
    return call;
}

// The generated tail of sub_826B02B0 from the C7A8 reservation through the
// epilogue, with the SDK export modelled from its source: it stores dest_count
// big-endian 0x80000000 dwords at dest and returns dest_count.
Owner runOwner(Guest& guest, const Layout& l, const Opts& opts = {})
{
    Owner owner;
    auto  ctx = makeContext(l);
    // RtlFillMemoryUlong ran earlier over the output; the fixture poisons it.
    // C7A8(G, 220) returns the reservation.
    owner.c7a8.emplace_back(l.g, 220);
    // The prologue: __savegprlr_25 stores r25..r31 as doublewords at -64(r1) and the LR word at -8(r1)
    // of the caller's frame; stwu then links the backchain and drops r1 by 2464.
    owner.callerSp = l.sp + 2464;
    for (unsigned k = 0; k < 7; ++k)
    {
        guest.put32(owner.callerSp - 64 + 8 * k, static_cast<std::uint32_t>(callerRegister(k) >> 32));
        guest.put32(owner.callerSp - 60 + 8 * k, static_cast<std::uint32_t>(callerRegister(k)));
    }
    guest.put32(owner.callerSp - 8, kCallerLr);
    guest.put32(l.sp, owner.callerSp);
    ctx.lr             = kPreCallLr;
    unsigned   lrReads = 0;
    const auto readLr  = [&]
    {
        ++lrReads;
        return ctx.lr;
    };
    if (opts.tweak)
        opts.tweak(guest, ctx);
    owner.atHook     = ctx;
    owner.beforeHook = importCall(guest, ctx);

    // 0x826B042C: the hook after mr r3,r25.
    owner.replaced  = opts.hooks && ReplaceNativeVideoScalerCall(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10, ctx.r30, ctx.r31);
    owner.afterHook = ctx;
    if (!owner.replaced)
    {
        // 0x826B0430: bl VdInitializeScalerCommandBuffer.
        const auto call = importCall(guest, ctx);
        owner.imports.push_back(call);
        const std::uint32_t dest = call.slot[2], count = call.slot[3];
        // The real export would store a huge count too; the model stops at a size the fixture can hold.
        for (std::uint32_t i = 0; i < std::min(count, 4096U); ++i)
            guest.put32(dest + 4 * i, opts.sdkFault == 2 && i == 77 ? 0x80000001 : 0x80000000);
        if (opts.sdkFault == 3)
            guest.put32(ctx.r1.u32 + 188, 0);
        ctx.r3.u64 = opts.sdkFault == 1 ? count - 1 : count;
        ctx.lr     = 0x826B0434;
    }
    // 0x826B0434.
    owner.atReturn = ctx;
    if (opts.pause)
        opts.pause();
    ObserveNativeVideoScalerReturn(ctx.r1, ctx.r3, ctx.r30, ctx.r31);
    ctx.r29.u64 = (static_cast<std::uint32_t>(ctx.r3.u64) << 2) & 0xFFFFFFFCU;
    // The memcpy call: bl sets LR, nothing before it read LR.
    owner.r29AtCopy            = ctx.r29.u64;
    owner.lrReadsBeforeCopy    = lrReads;
    ctx.lr                     = 0x826B0448;
    const std::uint32_t source = ctx.r1.u32 + 192, dest = ctx.r30.u32 + 4;
    const std::uint32_t n = std::uint32_t(ctx.r29.u64);
    ctx.r3.u64            = dest;
    ctx.r4.u64            = source;
    ctx.r5.u64            = n;
    const auto copy       = [&]
    {
        (void)readLr();
        for (std::uint32_t i = 0; i < std::min(n, 4096U); ++i)
            guest.put8(dest + i, guest.get8(source + i));
        if (opts.copyFault == 1)
            guest.put32(dest + 40, 1);
        owner.trace.push_back("copy_complete");
    };
    // 0x826B044C: stw of G+0x30.
    const auto publish = [&]
    {
        owner.trace.push_back("cursor_publish");
        guest.put32(ctx.r31.u32 + 0x30, ctx.r30.u32 + std::uint32_t(ctx.r29.u64) - (opts.cursorFault == 1 ? 4 : 0));
    };
    copy();
    publish();
    ObserveNativeVideoScalerCopy(ctx.r1, ctx.r30, ctx.r31);
    // The epilogue: addi r1,r1,2464, then __restgprlr_25 reloads r25..r31 and the LR word from the frame.
    ctx.r1.s64 = ctx.r1.s64 + 2464;
    PPCRegister* const saved[7]{ &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31 };
    for (unsigned k = 0; k < 7; ++k)
        saved[k]->u64 = (std::uint64_t{ guest.get32(ctx.r1.u32 - 64 + 8 * k) } << 32) | guest.get32(ctx.r1.u32 - 60 + 8 * k);
    ctx.r12.u64 = guest.get32(ctx.r1.u32 - 8);
    ctx.lr      = ctx.r12.u64;
    owner.final = ctx;
    return owner;
}

struct Counts
{
    VideoScalerStubCounts counts;

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
    return { TakeVideoScalerStubCounts() };
}

bool nopOutput(Guest& guest, const Layout& l)
{
    for (std::uint32_t i = 0; i < 800; ++i)
        if (guest.get8(l.sp + 192 + i) != ((i & 3) == 0 ? 0x80 : 0x00))
            return false;
    return true;
}

bool untouched(Guest& guest, const Layout& l)
{
    return guest.get32(l.sp + 188) == kBelow && guest.get32(l.sp + 992) == kAbove && guest.get32(l.r) == kHeader && guest.get32(l.r + 804) == kTail;
}

// ---------------------------------------------------------------------------

void testConstants()
{
    static_assert(kVideoScalerHookAddress == 0x826B042C && kVideoScalerImportAddress == 0x826B0430 && kVideoScalerReturnAddress == 0x826B0434);
    static_assert(kVideoScalerCopyAddress == 0x826B044C && kVideoScalerWords == 200 && kVideoScalerBytes == 800 && kVideoScalerNop == 0x80000000);
    static_assert(kVideoScalerOutputOffset + kVideoScalerBytes == kVideoScalerAboveSentinel && kVideoScalerBelowSentinel + 4 == kVideoScalerOutputOffset);
    UNSCOPED_INFO("ten refusal reasons and five mismatch kinds");
    CHECK((kVideoScalerRefusalCount == 10 && kVideoScalerMismatchCount == 5));
    const char* refusals[]   = { "services", "diagnostics", "stack", "shape", "unreadable", "unwritable", "reservation", "alias", "reentrant", "unpaired" };
    const char* mismatches[] = { "output", "return", "copy", "cursor", "sentinel" };
    for (std::size_t i = 0; i < 10; ++i)
    {
        UNSCOPED_INFO("refusal name order");
        CHECK(kVideoScalerRefusalNames[i] == refusals[i]);
    }
    for (std::size_t i = 0; i < 5; ++i)
    {
        UNSCOPED_INFO("mismatch name order");
        CHECK(kVideoScalerMismatchNames[i] == mismatches[i]);
    }
    UNSCOPED_INFO("both options default to off");
    CHECK((!REXCVAR_GET(native_video_scaler_stub) && !REXCVAR_GET(native_video_scaler_exit_check)));
    UNSCOPED_INFO("the final drain waits at most 10 ms");
    CHECK(kVideoScalerShutdownWait == std::chrono::milliseconds(10));
}

void testDeriveAndApply()
{
    Guest guest;
    setup(guest, L0);
    const auto      memory = guest.memory();
    const auto      call   = makeCall(makeContext(L0));
    VideoScalerPlan plan;
    const auto      refusal = DeriveVideoScalerStub(memory, call, VideoScalerState{ true, false, {} }, plan);
    UNSCOPED_INFO("the owner's call shape derives");
    CHECK(!refusal);
    UNSCOPED_INFO("plan identities");
    CHECK((plan.sp == L0.sp && plan.device == L0.g && plan.reservation == L0.r));
    UNSCOPED_INFO("plan addresses");
    CHECK((plan.output == L0.sp + 192 && plan.copy == L0.r + 4 && plan.cursor == L0.g + 0x30 && plan.cursorValue == L0.r + 800));
    UNSCOPED_INFO("baseline words");
    CHECK((plan.baseline.valid && plan.baseline.below == kBelow && plan.baseline.above == kAbove && plan.baseline.header == kHeader && plan.baseline.tail == kTail));
    UNSCOPED_INFO("derive stores nothing");
    CHECK((guest.stores.empty() && guest.writeAttempts == 0));
    for (std::uint32_t i = 0; i < 200; ++i)
    {
        UNSCOPED_INFO("derive leaves the poisoned output");
        CHECK(guest.get32(L0.sp + 192 + 4 * i) == kOutputPoison);
    }
    VideoScalerPlan again;
    UNSCOPED_INFO("derive is repeatable");
    CHECK((!DeriveVideoScalerStub(memory, call, VideoScalerState{ true, false, {} }, again) && again.output == plan.output && again.cursorValue == plan.cursorValue));

    UNSCOPED_INFO("apply completes");
    CHECK(ApplyVideoScalerStub(memory, plan) == VideoScalerApply::Done);
    UNSCOPED_INFO("exactly 200 stores");
    CHECK(guest.stores.size() == 200);
    bool inOrder = guest.stores.size() == 200;
    for (std::size_t i = 0; inOrder && i < 200; ++i)
        inOrder = guest.stores[i].first == L0.sp + 192 + 4 * i && guest.stores[i].second == 0x80000000;
    UNSCOPED_INFO("stores ascend from SP+192 with 0x80000000");
    CHECK(inOrder);
    UNSCOPED_INFO("all 800 bytes are big-endian 80 00 00 00");
    CHECK(nopOutput(guest, L0));
    UNSCOPED_INFO("sentinels and ring words are unchanged");
    CHECK(untouched(guest, L0));
    UNSCOPED_INFO("filter parameters and coefficients are unchanged");
    CHECK((guest.get32(L0.sp + 84) == L0.sp + 172 && guest.get32(L0.sp + 156) == 0xF1000000 && guest.get32(L0.sp + 172) == 0xC0000000));
}

void testSlotDecoding()
{
    const auto derive = [](Guest& guest, const VideoScalerCall& call)
    {
        VideoScalerPlan plan;
        return DeriveVideoScalerStub(guest.memory(), call, VideoScalerState{ true, false, {} }, plan);
    };
    const auto call = makeCall(makeContext(L0));
    {
        Guest guest;
        setup(guest, L0);
        UNSCOPED_INFO("poison in the high words and the unused slots does not matter");
        CHECK(!derive(guest, call));
    }
    // The values only in the high words of the 8-byte slots never select the shape.
    {
        Guest guest;
        setup(guest, L0);
        guest.put32(L0.sp + 96, L0.sp + 192);
        guest.put32(L0.sp + 104, 200);
        guest.put32(L0.sp + 100, L0.sp + 196);
        guest.put32(L0.sp + 108, 199);
        UNSCOPED_INFO("values in the high words are not the arguments");
        CHECK(derive(guest, call) == R::Shape);
        UNSCOPED_INFO("no store");
        CHECK(guest.writeAttempts == 0);
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.put32(L0.sp + 104, L0.sp + 192);
        guest.put32(L0.sp + 112, 200);
        guest.put32(L0.sp + 100, 0);
        guest.put32(L0.sp + 108, 0);
        UNSCOPED_INFO("SP+104 and SP+112 are not the destination and count");
        CHECK(derive(guest, call) == R::Shape);
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.unreadable.insert(L0.sp + 108);
        UNSCOPED_INFO("an unreadable argument slot is a stack refusal");
        CHECK(derive(guest, call) == R::Stack);
    }
}

void testShapeRefusals()
{
    struct Case
    {
        const char*                              name;
        std::function<void(Guest&, PPCContext&)> tweak;
    };

    const Case cases[] = {
        { "count 199", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 108, 199);
          } },
        { "count 201", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 108, 201);
          } },
        { "count UINT32_MAX", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 108, 0xFFFFFFFF);
          } },
        { "destination SP+196", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 100, c.r1.u32 + 196);
          } },
        { "destination SP+188", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 100, c.r1.u32 + 188);
          } },
        { "vertical params pointer", [](Guest&, PPCContext& c)
          {
              c.r9.u64 = c.r1.u32 + 160;
          } },
        { "horizontal params slot", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 84, c.r1.u32 + 176);
          } },
        { "unk9 slot", [](Guest& g, PPCContext& c)
          {
              g.put32(c.r1.u32 + 92, c.r1.u32 + 996);
          } },
    };
    for (const auto& c : cases)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, false, true);
        const auto owner  = runOwner(guest, L0, Opts{ .tweak = c.tweak });
        const auto counts = take();
        UNSCOPED_INFO(std::string(c.name) + ": the import runs once");
        CHECK((!owner.replaced && owner.imports.size() == 1));
        UNSCOPED_INFO(std::string(c.name) + ": the import sees the original inputs");
        CHECK((owner.imports.size() == 1 && owner.imports[0] == owner.beforeHook));
        UNSCOPED_INFO(std::string(c.name) + ": the hook changes no register");
        CHECK(sameContext(owner.atHook, owner.afterHook));
        UNSCOPED_INFO(std::string(c.name) + ": no stub store");
        CHECK((guest.stores.empty() && guest.writeAttempts == 0));
        UNSCOPED_INFO(std::string(c.name) + ": one shape refusal");
        CHECK((counts.refusal(R::Shape) == 1 && counts.refusals() == 1 && counts.counts.original == 1 && counts.counts.consumed == 0 && counts.counts.enter == 1));
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

    const auto split = [](Guest& g, std::size_t index, std::uint32_t at, bool read, bool write)
    {
        auto region = g.regions[index];
        auto first  = region;
        first.size  = at - region.lo;
        auto second = region;
        second.lo   = at;
        second.size = region.size - first.size;
        second.identity += first.size;
        second.read      = read;
        second.write     = write;
        g.regions[index] = first;
        g.regions.push_back(second);
    };
    const Case cases[] = {
        { "device not validated", R::Services, false, [](Guest&, PPCContext&)
          {
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(L0.g + 4);
          } },
        { "exit check forces the import", R::Diagnostics, true, nullptr },
        { "SP misaligned", R::Stack, false, [](Guest&, PPCContext& c)
          {
              c.r1.u64 = L0.sp + 8;
          } },
        // The argument slots are readable here, so only the frame arithmetic can refuse it.
        { "SP wraps", R::Stack, false, [](Guest& g, PPCContext& c)
          {
              g.add({ 0xFFFFF000, 0x1000, (std::uint64_t{ 1 } << 32) + 0xFFFFF000 });
              c.r1.u64 = 0xFFFFF660;
              g.put32(0xFFFFF660 + 84, 0xFFFFF660 + 172);
              g.put32(0xFFFFF660 + 92, 0xFFFFF660 + 992);
              g.put32(0xFFFFF660 + 100, 0xFFFFF660 + 192);
              g.put32(0xFFFFF660 + 108, 200);
              c.r9.u64 = 0xFFFFF660 + 156;
          } },
        { "argument slot unreadable", R::Stack, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp + 100);
          } },
        { "shape", R::Shape, false, [](Guest& g, PPCContext&)
          {
              g.put32(L0.sp + 108, 199);
          } },
        { "below sentinel unreadable", R::Unreadable, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp + 188);
          } },
        { "above sentinel unreadable", R::Unreadable, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.sp + 992);
          } },
        { "output read only", R::Unwritable, false, [](Guest& g, PPCContext&)
          {
              g.regions[0].write = false;
          } },
        { "output last page not writable", R::Unwritable, false, [&split](Guest& g, PPCContext&)
          {
              split(g, 0, L0.sp + 192 + 400, true, false);
          } },
        { "reservation null", R::Reservation, false, [](Guest&, PPCContext& c)
          {
              c.r30.u64 = 0;
          } },
        { "reservation misaligned", R::Reservation, false, [](Guest&, PPCContext& c)
          {
              c.r30.u64 = L0.r + 2;
          } },
        { "reservation wraps", R::Reservation, false, [](Guest&, PPCContext& c)
          {
              c.r30.u64 = 0xFFFFFFFC;
          } },
        { "copy destination not writable", R::Reservation, false, [](Guest& g, PPCContext&)
          {
              g.regions[2].write = false;
          } },
        { "copy range last page not writable", R::Reservation, false, [&split](Guest& g, PPCContext&)
          {
              split(g, 2, L0.r + 400, true, false);
          } },
        { "cursor not writable", R::Reservation, false, [](Guest& g, PPCContext&)
          {
              g.regions[1].write = false;
          } },
        { "header unreadable", R::Reservation, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.r);
          } },
        { "tail unreadable", R::Reservation, false, [](Guest& g, PPCContext&)
          {
              g.unreadable.insert(L0.r + 804);
          } },
        { "device overlaps the output", R::Alias, false, [](Guest& g, PPCContext&)
          {
              g.regions[1].identity = (std::uint64_t{ 1 } << 32) + L0.sp - 0x100;
          } },
        { "device aliases the copy physically", R::Alias, false, [](Guest& g, PPCContext&)
          {
              g.regions[1].identity = 0x10000;
          } },
        { "output aliases the copy physically", R::Alias, false, [](Guest& g, PPCContext&)
          {
              g.regions[0].identity = 0x10014 - (L0.sp + 192 - g.regions[0].lo);
          } },
        { "frame range crosses a mapping", R::Alias, false, [&split](Guest& g, PPCContext&)
          {
              split(g, 0, L0.sp + 1500, true, true);
          } },
        { "device range wraps", R::Alias, false, [](Guest& g, PPCContext& c)
          {
              g.add({ 0xFFFFA000, 0x1000, (std::uint64_t{ 1 } << 32) + 0xFFFFA000 });
              c.r31.u64 = 0xFFFFA400;
              rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0xFFFFA400);
          } },
        { "no observation record", R::Unpaired, false, [](Guest&, PPCContext&)
          {
              SetVideoScalerPendingLimitForTesting(0);
          } },
    };
    for (const auto& c : cases)
    {
        const std::string name = c.name;
        Guest             guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, c.check, true);
        (void)take();
        const auto owner  = runOwner(guest, L0, Opts{ .tweak = c.tweak });
        const auto counts = take();
        UNSCOPED_INFO(name + ": not replaced");
        CHECK(!owner.replaced);
        UNSCOPED_INFO(name + ": the import runs exactly once");
        CHECK(owner.imports.size() == 1);
        UNSCOPED_INFO(name + ": the import sees the original inputs");
        CHECK((owner.imports.size() == 1 && owner.imports[0] == owner.beforeHook));
        UNSCOPED_INFO(name + ": registers unchanged by the hook");
        CHECK(sameContext(owner.atHook, owner.afterHook));
        UNSCOPED_INFO(name + ": no stub store before the refusal");
        CHECK((guest.stores.empty() && guest.writeAttempts == 0));
        UNSCOPED_INFO(name + ": no terminal");
        CHECK(guest.terminals.empty());
        UNSCOPED_INFO(name + ": exactly that refusal");
        CHECK((counts.refusal(c.reason) == 1 && counts.refusals() == 1));
        UNSCOPED_INFO(name + ": original counted once");
        CHECK((counts.counts.consumed == 0 && counts.counts.original == 1 && counts.counts.enter == 1));
        SetVideoScalerPendingLimitForTesting(kVideoScalerMaxPending);
    }
    // Reentrant is covered by testObservationPairing; unpublished services run nothing.
}

void testWriteFault()
{
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        guest.failAt      = 2;
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a fault after the output started still returns true");
        CHECK(owner.replaced);
        UNSCOPED_INFO("the import never runs over partial output");
        CHECK(owner.imports.empty());
        UNSCOPED_INFO("the terminal callback fires once");
        CHECK(guest.terminals.size() == 1);
        UNSCOPED_INFO("write_failed counted, no original");
        CHECK((counts.counts.writeFailed == 1 && counts.counts.original == 0 && counts.counts.consumed == 0));
        UNSCOPED_INFO("one store landed before the fault");
        CHECK(guest.stores.size() == 1);
        UNSCOPED_INFO("the failure is logged");
        CHECK(!guest.logs.empty());
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        guest.failAt      = 3;
        guest.throwAtFail = true;
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a throwing store is a terminal failure too");
        CHECK((owner.replaced && owner.imports.empty() && guest.terminals.size() == 1 && counts.counts.writeFailed == 1));
    }
    // A logger that throws after the fault must not turn the fail-stop into the import.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        guest.failAt      = 2;
        guest.throwOnLog  = true;
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a throwing logger after the fault still never runs the import");
        CHECK((owner.replaced && owner.imports.empty()));
        UNSCOPED_INFO("the terminal callback still fires once");
        CHECK(guest.terminals.size() == 1);
        UNSCOPED_INFO("write_failed counted once, no original");
        CHECK((counts.counts.writeFailed == 1 && counts.counts.original == 0 && counts.counts.consumed == 0));
        UNSCOPED_INFO("no store after the fault");
        CHECK(guest.stores.size() == 1);
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        guest.failAt      = 1;
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("a fault on the first store changed nothing: the import runs");
        CHECK((!owner.replaced && owner.imports.size() == 1 && guest.terminals.empty()));
        UNSCOPED_INFO("counted as unwritable");
        CHECK((counts.refusal(R::Unwritable) == 1 && counts.counts.writeFailed == 0 && counts.counts.original == 1));
    }
}

void testServicesAndLease()
{
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(true, false, false);
        const auto owner = runOwner(guest, L0);
        UNSCOPED_INFO("no published services: the import runs");
        CHECK((!owner.replaced && owner.imports.size() == 1));
        const auto counts = take();
        UNSCOPED_INFO("unpublished services count and read nothing");
        CHECK((counts.counts.enter == 0 && counts.refusals() == 0 && guest.reads == 0 && guest.identities == 0));
        UNSCOPED_INFO("no store");
        CHECK(guest.stores.empty());
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        SetRingStubLeaseProbe([]() noexcept
                              {
                                  rerevved::gpu::guest_d3d::ringStubsActive.store(false, std::memory_order_seq_cst);
                              });
        const auto owner = runOwner(guest, L0);
        SetRingStubLeaseProbe(nullptr);
        const auto counts = take();
        UNSCOPED_INFO("a withdrawal before the lease runs the import");
        CHECK((!owner.replaced && owner.imports.size() == 1 && guest.stores.empty()));
        UNSCOPED_INFO("counted as services");
        CHECK(counts.refusal(R::Services) == 1);
    }
    // A withdrawal cannot finish while the admitted writer holds its lease.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars              cvars(true, false, true);
        std::promise<void> reached, release;
        auto               released = release.get_future().share();
        std::atomic<bool>  withdrawn{ false };
        guest.onStore = [&](std::size_t index)
        {
            if (index == 3)
            {
                reached.set_value();
                released.wait();
            }
        };
        Owner       owner;
        std::thread writer([&]
                           {
                               owner = runOwner(guest, L0);
                           });
        reached.get_future().wait();
        std::thread withdrawer([&]
                               {
                                   rerevved::gpu::guest_d3d::WithdrawRingStubServices();
                                   withdrawn = true;
                               });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        UNSCOPED_INFO("the withdrawal waits for the writer's lease");
        CHECK(!withdrawn.load());
        release.set_value();
        writer.join();
        withdrawer.join();
        UNSCOPED_INFO("the withdrawal finishes after the writes");
        CHECK(withdrawn.load());
        UNSCOPED_INFO("the admitted writer finished all 200 stores");
        CHECK((owner.replaced && guest.stores.size() == 200 && nopOutput(guest, L0)));
        (void)take();
    }
}

void testDisabledWork()
{
    PPCContext ctx = makeContext(L0);
    Guest      guest;
    setup(guest, L0);
    guest.install();
    for (int mode = 0; mode < 2; ++mode)
    {
        // Mode 0: both options off with services published. Mode 1: the stub on, services not published.
        Cvars         cvars(mode == 1, false, mode == 0);
        const auto    touches   = VideoScalerThreadTouchesForTesting();
        bool          replaced  = false;
        std::uint64_t allocated = 0;
        {
            rerevved::tests::allocation::ScopedAllocationCount count;
            for (int i = 0; i < 1000; ++i)
            {
                replaced |= ReplaceNativeVideoScalerCall(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10, ctx.r30, ctx.r31);
                replaced |= ReplaceVideoScalerCall(makeCall(ctx), ctx.r3.u64);
                ObserveNativeVideoScalerReturn(ctx.r1, ctx.r3, ctx.r30, ctx.r31);
                ObserveNativeVideoScalerCopy(ctx.r1, ctx.r30, ctx.r31);
            }
            allocated = count.Count();
        }
        const auto counts = take();
        UNSCOPED_INFO("disabled: nothing is replaced");
        CHECK(!replaced);
        UNSCOPED_INFO("disabled: no allocation");
        CHECK(allocated == 0);
        UNSCOPED_INFO("disabled: no memory access");
        CHECK((guest.reads == 0 && guest.identities == 0 && guest.writeAttempts == 0));
        UNSCOPED_INFO("disabled: no thread bookkeeping");
        CHECK(VideoScalerThreadTouchesForTesting() == touches);
        UNSCOPED_INFO("disabled: nothing is counted");
        CHECK((counts.counts.enter == 0 && counts.refusals() == 0 && counts.counts.original == 0 && counts.counts.exitUnpaired == 0 && counts.counts.pending == 0));
        if (mode == 0)
        {
            UNSCOPED_INFO("disabled: the drains return no fields");
            CHECK((ReportVideoScalerStub().empty() && ReportVideoScalerStub(true).empty()));
        }
        UNSCOPED_INFO("disabled: nothing is logged");
        CHECK((guest.logs.empty() && guest.terminals.empty()));
        UNSCOPED_INFO("disabled: registers unchanged");
        CHECK(sameContext(ctx, makeContext(L0)));
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
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("exit check: the import runs exactly once");
        CHECK((!owner.replaced && owner.imports.size() == 1));
        UNSCOPED_INFO("exit check: consumed 0, diagnostics refusal");
        CHECK((counts.counts.consumed == 0 && counts.counts.original == 1 && counts.refusal(R::Diagnostics) == 1));
        UNSCOPED_INFO("exit check: the matched observation is ok");
        CHECK((counts.counts.exitOk == 1 && counts.mismatches() == 0 && counts.counts.exitUnreadable == 0 && counts.counts.exitUnpaired == 0));
        UNSCOPED_INFO("exit check: the stub stores nothing");
        CHECK(guest.stores.empty());
    }
    // The exit check alone observes without any refusal.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars      cvars(false, true, false);
        const auto owner  = runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("check only: the import runs");
        CHECK((!owner.replaced && owner.imports.size() == 1 && guest.stores.empty()));
        UNSCOPED_INFO("check only: exit_ok without published services");
        CHECK((counts.counts.enter == 1 && counts.counts.original == 1 && counts.refusals() == 0 && counts.counts.exitOk == 1));
        UNSCOPED_INFO("check only: nothing outstanding");
        CHECK(counts.counts.pending == 0);
    }

    struct Fault
    {
        const char* name;
        Opts        opts;
        M           kind;
    };

    const Fault faults[] = {
        { "wrong count", Opts{ .sdkFault = 1 }, M::Return },
        { "wrong output word", Opts{ .sdkFault = 2 }, M::Output },
        { "clobbered sentinel", Opts{ .sdkFault = 3 }, M::Sentinel },
        { "wrong copied word", Opts{ .copyFault = 1 }, M::Copy },
        { "wrong cursor", Opts{ .cursorFault = 1 }, M::Cursor },
    };
    for (const auto& fault : faults)
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(false, true, false);
        (void)runOwner(guest, L0, fault.opts);
        const auto counts = take();
        UNSCOPED_INFO(std::string(fault.name) + ": counted and not ok");
        CHECK((counts.mismatch(fault.kind) == 1 && counts.counts.exitOk == 0));
    }
    {
        Guest guest;
        setup(guest, L0);
        guest.unreadable.insert(L0.sp + 992);
        guest.install();
        Cvars cvars(false, true, false);
        (void)runOwner(guest, L0);
        const auto counts = take();
        UNSCOPED_INFO("an unreadable sentinel is inconclusive");
        CHECK((counts.counts.exitUnreadable == 1 && counts.counts.exitOk == 0));
    }
}

void testObservationPairing()
{
    const auto pausing = [](std::promise<void>& paused, std::shared_future<void> resumed)
    {
        return Opts{ .pause = [&paused, resumed]
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
                              (void)runOwner(guest, L0, pausing(paused, resumed));
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
                              (void)runOwner(guest, L0, pausing(paused, resumed));
                                 });
        paused.get_future().wait();
        const auto second = runOwner(guest, L1);
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
    // A call nested below a live frame poisons both and is never exit_ok.
    {
        Guest guest;
        setup(guest, L0);
        setup(guest, L2);
        guest.install();
        Cvars      cvars(true, false, true);
        Owner      nested;
        const auto outer  = runOwner(guest, L0, Opts{ .pause = [&]
                                                      {
                                                         nested = runOwner(guest, L2);
                                                      } });
        const auto counts = take();
        UNSCOPED_INFO("the nested call runs the import once");
        CHECK((outer.replaced && !nested.replaced && nested.imports.size() == 1));
        UNSCOPED_INFO("the nested call is refused as reentrant");
        CHECK((counts.refusal(R::Reentrant) == 1 && counts.refusals() == 1));
        UNSCOPED_INFO("a nested pair is never ok");
        CHECK(counts.counts.exitOk == 0);
        UNSCOPED_INFO("three unpaired observations, nothing outstanding");
        CHECK((counts.counts.exitUnpaired == 3 && counts.counts.pending == 0));
    }
    // An abandoned call is closed as unpaired by the next call on the thread.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars         cvars(true, false, true);
        std::uint64_t r3 = 0;
        UNSCOPED_INFO("the first call is consumed");
        CHECK((ReplaceVideoScalerCall(makeCall(makeContext(L0)), r3) && r3 == 200));
        UNSCOPED_INFO("its observations never come");
        CHECK(take().counts.pending == 1);
        const auto owner  = runOwner(guest, L0);
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
        std::uint64_t r3 = 0;
        UNSCOPED_INFO("one outstanding call");
        CHECK(ReplaceVideoScalerCall(makeCall(makeContext(L0)), r3));
        SetVideoScalerPendingLimitForTesting(1);
        Owner       other;
        std::thread t([&]
                      {
                          other = runOwner(guest, L1);
                      });
        t.join();
        SetVideoScalerPendingLimitForTesting(kVideoScalerMaxPending);
        const auto counts = take();
        UNSCOPED_INFO("the bound refuses the new call");
        CHECK((!other.replaced && other.imports.size() == 1 && counts.refusal(R::Unpaired) == 1));
        auto ctx = makeContext(L0);
        ObserveNativeVideoScalerCopy(ctx.r1, ctx.r30, ctx.r31);
        const auto closed = take();
        UNSCOPED_INFO("an incomplete pair is unpaired, never ok");
        CHECK((closed.counts.exitOk == 0 && closed.counts.exitUnpaired == 1 && closed.counts.pending == 0));
    }
    // An observation with no call is unpaired.
    {
        Guest guest;
        setup(guest, L0);
        guest.install();
        Cvars cvars(true, false, true);
        auto  ctx = makeContext(L0);
        ObserveNativeVideoScalerReturn(ctx.r1, ctx.r3, ctx.r30, ctx.r31);
        ObserveNativeVideoScalerCopy(ctx.r1, ctx.r30, ctx.r31);
        const auto counts = take();
        UNSCOPED_INFO("observations without a call are unpaired");
        CHECK((counts.counts.exitOk == 0 && counts.counts.exitUnpaired == 2 && counts.counts.pending == 0));
    }
}

void testContinuation()
{
    Guest sdk, stub;
    setup(sdk, L0);
    setup(stub, L0);
    sdk.install();
    Owner a, b;
    {
        Cvars off(false, false, false);
        a = runOwner(sdk, L0, Opts{ .hooks = false });
    }
    stub.install();
    {
        Cvars on(true, false, true);
        b                 = runOwner(stub, L0);
        const auto counts = take();
        UNSCOPED_INFO("the stub run is consumed and observed ok");
        CHECK((counts.counts.consumed == 1 && counts.counts.original == 0 && counts.refusals() == 0 && counts.counts.exitOk == 1));
    }
    UNSCOPED_INFO("the import runs only in the SDK run");
    CHECK((a.imports.size() == 1 && b.imports.empty() && b.replaced && !a.replaced));
    UNSCOPED_INFO("the full r3 is 200 at the return");
    CHECK((a.atReturn.r3.u64 == 200 && b.atReturn.r3.u64 == 200));
    auto bReturn = b.atReturn;
    bReturn.lr   = a.atReturn.lr;
    UNSCOPED_INFO("every register but LR is equal at 0x826B0434");
    CHECK(sameContext(a.atReturn, bReturn));
    UNSCOPED_INFO("LR is the pre-call value after the skipped call");
    CHECK((a.atReturn.lr == 0x826B0434 && b.atReturn.lr == kPreCallLr));
    UNSCOPED_INFO("nothing reads LR before the memcpy call");
    CHECK((b.lrReadsBeforeCopy == 0 && a.lrReadsBeforeCopy == 0));
    UNSCOPED_INFO("the whole context reconverges after the epilogue");
    CHECK(sameContext(a.final, b.final));
    for (const auto* run : { &a, &b })
    {
        UNSCOPED_INFO("the epilogue restores SP to the caller's frame");
        CHECK((run->callerSp == L0.sp + 2464 && run->final.r1.u64 == run->callerSp));
        UNSCOPED_INFO("the epilogue restores the nonvolatile r25 through r31 from the saved frame");
        CHECK((run->final.r25.u64 == callerRegister(0) && run->final.r26.u64 == callerRegister(1) && run->final.r27.u64 == callerRegister(2) &&
               run->final.r28.u64 == callerRegister(3) && run->final.r29.u64 == callerRegister(4) && run->final.r30.u64 == callerRegister(5) &&
               run->final.r31.u64 == callerRegister(6)));
        UNSCOPED_INFO("the epilogue restores LR from the saved word");
        CHECK((run->final.lr == kCallerLr && run->final.r12.u64 == kCallerLr));
        UNSCOPED_INFO("the body held its own values and frame");
        CHECK((run->atReturn.r25.u64 != callerRegister(0) && run->atReturn.r29.u64 != callerRegister(4) && run->atReturn.r1.u64 == L0.sp));
    }
    UNSCOPED_INFO("one C7A8(G, 220) request");
    CHECK((a.c7a8.size() == 1 && a.c7a8 == b.c7a8 && b.c7a8[0] == std::make_pair(L0.g, 220U)));
    UNSCOPED_INFO("the guest memory is identical to the SDK-backed run");
    CHECK(sdk.bytes == stub.bytes);
    const std::vector<std::string> expected{ "copy_complete", "cursor_publish" };
    UNSCOPED_INFO("the copy completes before the cursor is published");
    CHECK((a.trace == expected && b.trace == expected));
    // An independent oracle for the retained effects.
    UNSCOPED_INFO("R and G are preserved");
    CHECK((b.atReturn.r30.u64 == L0.r && b.atReturn.r31.u64 == L0.g));
    UNSCOPED_INFO("SP is preserved");
    CHECK(b.atReturn.r1.u64 == L0.sp);
    UNSCOPED_INFO("800 NOP bytes at SP+192");
    CHECK(nopOutput(stub, L0));
    UNSCOPED_INFO("200 stub stores");
    CHECK(stub.stores.size() == 200);
    bool copied = true;
    for (std::uint32_t i = 0; i < 800; ++i)
        copied = copied && stub.get8(L0.r + 4 + i) == ((i & 3) == 0 ? 0x80 : 0x00);
    UNSCOPED_INFO("the copy covers [R+4, R+804)");
    CHECK(copied);
    UNSCOPED_INFO("the ring header and the word after the copy are preserved");
    CHECK((stub.get32(L0.r) == kHeader && stub.get32(L0.r + 804) == kTail));
    UNSCOPED_INFO("G+0x30 is R+800");
    CHECK(stub.get32(L0.g + 0x30) == L0.r + 800);
    UNSCOPED_INFO("G+0x34 and G+0x38 are preserved");
    CHECK((stub.get32(L0.g + 0x34) == 0x64000034 && stub.get32(L0.g + 0x38) == 0x64000038));
    UNSCOPED_INFO("r29 is the byte count 800 after the rlwinm");
    CHECK((b.atReturn.r29.u64 != 800 && b.r29AtCopy == 800));
    UNSCOPED_INFO("stack sentinels preserved");
    CHECK((stub.get32(L0.sp + 188) == kBelow && stub.get32(L0.sp + 992) == kAbove));
    UNSCOPED_INFO("filter inputs preserved");
    CHECK((stub.get32(L0.sp + 156) == 0xF1000000 && stub.get32(L0.sp + 172) == 0xC0000000));
}

void testHookAbi()
{
    static_assert(std::is_same_v<decltype(&ReplaceNativeVideoScalerCall),
                                 bool (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoScalerReturn), void (*)(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)>);
    static_assert(std::is_same_v<decltype(&ObserveNativeVideoScalerCopy), void (*)(PPCRegister&, PPCRegister&, PPCRegister&)>);
    Guest guest;
    setup(guest, L0);
    guest.install();
    Cvars      cvars(true, false, true);
    auto       ctx    = makeContext(L0);
    const auto before = ctx;
    UNSCOPED_INFO("the hook replaces the call");
    CHECK(ReplaceNativeVideoScalerCall(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10, ctx.r30, ctx.r31));
    UNSCOPED_INFO("the full r3 is 200, stale high bits cleared");
    CHECK(ctx.r3.u64 == 200);
    auto expected   = before;
    expected.r3.u64 = 200;
    UNSCOPED_INFO("only r3 changed");
    CHECK(sameContext(ctx, expected));
    ObserveNativeVideoScalerReturn(ctx.r1, ctx.r3, ctx.r30, ctx.r31);
    UNSCOPED_INFO("the return observer changes no register");
    CHECK(sameContext(ctx, expected));
    // Close the call so the thread has no outstanding record.
    ObserveNativeVideoScalerCopy(ctx.r1, ctx.r30, ctx.r31);
    UNSCOPED_INFO("the copy observer changes no register");
    CHECK(sameContext(ctx, expected));
    const auto counts = take();
    UNSCOPED_INFO("one consumed call, none outstanding");
    CHECK((counts.counts.consumed == 1 && counts.counts.pending == 0));
    // A wide stale r1/r30/r31 high half is a different call, not the owner's shape.
    auto wide = makeContext(L0);
    wide.r30.u64 |= 0x1111000000000000ULL;
    const auto copyOfWide = wide;
    UNSCOPED_INFO("the 32-bit registers decide");
    CHECK(ReplaceNativeVideoScalerCall(wide.r1, wide.r3, wide.r4, wide.r5, wide.r6, wide.r7, wide.r8, wide.r9, wide.r10, wide.r30, wide.r31));
    (void)copyOfWide;
    ObserveNativeVideoScalerReturn(wide.r1, wide.r3, wide.r30, wide.r31);
    ObserveNativeVideoScalerCopy(wide.r1, wide.r30, wide.r31);
    (void)take();
}

void testReport()
{
    VideoScalerStubCounts counts;
    counts.enter                                         = 7;
    counts.consumed                                      = 5;
    counts.original                                      = 2;
    counts.refusal[static_cast<std::size_t>(R::Shape)]   = 2;
    counts.exitOk                                        = 4;
    counts.mismatch[static_cast<std::size_t>(M::Cursor)] = 1;
    counts.pending                                       = 3;
    const auto fields                                    = FormatVideoScalerStubCounts(counts);
    UNSCOPED_INFO("the leading fields");
    CHECK(fields.find(" video_scaler_enter=7 video_scaler_consumed=5 video_scaler_original=2") == 0);
    UNSCOPED_INFO("every refusal has a field");
    CHECK((fields.find(" video_scaler_refusal_shape=2") != std::string::npos && fields.find(" video_scaler_refusal_services=0") != std::string::npos));
    UNSCOPED_INFO("write_failed and exit_ok");
    CHECK(fields.find(" video_scaler_write_failed=0 video_scaler_exit_ok=4") != std::string::npos);
    UNSCOPED_INFO("every mismatch has a field");
    CHECK((fields.find(" video_scaler_exit_mismatch_cursor=1") != std::string::npos && fields.find(" video_scaler_exit_mismatch_output=0") != std::string::npos));
    UNSCOPED_INFO("the tail fields");
    CHECK(fields.find(" video_scaler_exit_unreadable=0 video_scaler_exit_unpaired=0 video_scaler_pending=3") != std::string::npos);
    UNSCOPED_INFO("a periodic line has no final markers");
    CHECK(fields.find("final") == std::string::npos);
    counts.final = true;
    UNSCOPED_INFO("an outstanding call makes the final incomplete");
    CHECK(FormatVideoScalerStubCounts(counts).find(" video_scaler_final_incomplete=1 video_scaler_final_skipped=0") != std::string::npos);
    counts.pending = 0;
    UNSCOPED_INFO("a clean final");
    CHECK(FormatVideoScalerStubCounts(counts).find(" video_scaler_final_incomplete=0 video_scaler_final_skipped=0") != std::string::npos);

    static std::vector<std::string> logs;
    logs.clear();
    const VideoScalerReportOperations report{ FormatVideoScalerStubCounts, [](std::string_view message)
                                              {
                                                  logs.emplace_back(message);
                                              } };
    Cvars cvars(true, false, true);
    // Counts drain per window; the pending gauge stays.
    Guest guest;
    setup(guest, L0);
    guest.install();
    const auto owner  = runOwner(guest, L0);
    const auto window = ReportVideoScalerStub(false, report);
    UNSCOPED_INFO("the window reports the call");
    CHECK((window.find(" video_scaler_enter=1 video_scaler_consumed=1") == 0 && window.find("video_scaler_exit_ok=1") != std::string::npos));
    UNSCOPED_INFO("the next window starts at zero");
    CHECK(ReportVideoScalerStub(false, report).find(" video_scaler_enter=0 video_scaler_consumed=0") == 0);
    UNSCOPED_INFO("a periodic drain does not log");
    CHECK(logs.empty());
    const auto final = ReportVideoScalerStub(true, report);
    UNSCOPED_INFO("the shutdown final is logged");
    CHECK((!logs.empty() && logs.back() == final && final.find("video_scaler_final_incomplete=0") != std::string::npos));
    (void)owner;
    // A concurrent drain holding the lock does not stall shutdown.
    {
        logs.clear();
        auto& mutex = detail::VideoScalerReportMutex();
        mutex.lock();
        const auto  begun = std::chrono::steady_clock::now();
        std::string skipped;
        std::thread t([&]
                      {
                          skipped = ReportVideoScalerStub(true, report);
                      });
        t.join();
        const auto elapsed = std::chrono::steady_clock::now() - begun;
        mutex.unlock();
        UNSCOPED_INFO("the final drain is bounded");
        CHECK(elapsed < std::chrono::seconds(2));
        UNSCOPED_INFO("the skipped final says so");
        CHECK(skipped.find("video_scaler_final_incomplete=1 video_scaler_final_skipped=1") != std::string::npos);
        UNSCOPED_INFO("the skipped final is logged");
        CHECK((!logs.empty() && logs.back() == skipped));
        const auto next = ReportVideoScalerStub(true, report);
        UNSCOPED_INFO("the skip is reported by the next final");
        CHECK(next.find("video_scaler_final_skipped=1") != std::string::npos);
    }
}

// The real SDK export and the production memory adapter over the runtime's guest memory.
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
    const auto word = [&](std::uint32_t address) -> std::uint32_t
    {
        std::uint32_t raw = 0;
        std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), 4);
        return rex::byte_swap(raw);
    };
    const auto put = [&](std::uint32_t address, std::uint32_t value)
    {
        const auto raw = rex::byte_swap(value);
        std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), &raw, 4);
    };
    const auto virt = [&](std::uint32_t bytes)
    {
        return memory->SystemHeapAlloc(bytes, 0x1000, rex::memory::kSystemHeapVirtual);
    };
    const auto phys = [&](std::uint32_t bytes)
    {
        return memory->SystemHeapAlloc(bytes, 0x1000, rex::memory::kSystemHeapPhysical);
    };
    const std::uint32_t frameA = virt(0x1000), frameB = virt(0x1000), device = virt(0x6000), ring = phys(0x2000);
    UNSCOPED_INFO("guest allocations");
    CHECK((frameA && frameB && device && ring));
    if (!(frameA && frameB && device && ring))
        return;
    const std::uint32_t reservation = ring + 0x10;
    UNSCOPED_INFO("the virtual heap needs no host offset for the SDK call");
    CHECK(memory->TranslateVirtual<std::uint8_t*>(frameA) == memory->virtual_membase() + frameA);
    const auto fill = [&](std::uint32_t sp)
    {
        for (std::uint32_t offset = 0; offset < 2464; offset += 4)
            put(sp + offset, 0x5A000000 | offset);
        for (std::uint32_t i = 0; i < 200; ++i)
            put(sp + 192 + 4 * i, kOutputPoison);
        put(sp + 188, kBelow);
        put(sp + 992, kAbove);
        put(sp + 80, 0xBAD00000);
        put(sp + 88, 0xBAD00001);
        put(sp + 96, 0xBAD00002);
        put(sp + 104, 0xBAD00003);
        put(sp + 84, sp + 172);
        put(sp + 92, sp + 992);
        put(sp + 100, sp + 192);
        put(sp + 108, 200);
    };
    fill(frameA);
    fill(frameB);
    put(reservation, kHeader);
    for (std::uint32_t i = 0; i < 200; ++i)
        put(reservation + 4 + 4 * i, kCopyPoison);
    put(reservation + 804, kTail);

    // The SDK export on frame A.
    PPCContext sdk{};
    std::memset(&sdk, 0x35, sizeof(sdk));
    sdk.r1.u64  = frameA;
    sdk.r3.u64  = 0xFEED000000200010ULL;
    sdk.r4.u64  = 0x02D00500;
    sdk.r5.u64  = 0x00100008;
    sdk.r6.u64  = 0x02D00500;
    sdk.r7.u64  = 0x040002D0;
    sdk.r8.u64  = 7;
    sdk.r9.u64  = frameA + 156;
    sdk.r10.u64 = 7;
    __imp__VdInitializeScalerCommandBuffer(sdk, memory->virtual_membase());
    UNSCOPED_INFO("the SDK export returns the dword count");
    CHECK(sdk.r3.u32 == 200);
    std::vector<std::uint32_t> expected;
    for (std::uint32_t i = 0; i < 200; ++i)
        expected.push_back(word(frameA + 192 + 4 * i));
    UNSCOPED_INFO("the SDK export stores 200 NOP dwords");
    CHECK(std::all_of(expected.begin(), expected.end(), [](std::uint32_t v)
                      {
                          return v == 0x80000000;
                      }));
    UNSCOPED_INFO("the SDK export leaves the neighbours");
    CHECK((word(frameA + 188) == kBelow && word(frameA + 992) == kAbove));

    // The stub on frame B through the production adapter.
    SetVideoScalerOperationsForTesting(nullptr);
    Cvars           cvars(true, false, true, device);
    VideoScalerCall call;
    call.r1           = frameB;
    call.arguments[0] = 0xFEED000000200010ULL;
    call.arguments[1] = 0x02D00500;
    call.arguments[2] = 0x00100008;
    call.arguments[3] = 0x02D00500;
    call.arguments[4] = 0x040002D0;
    call.arguments[5] = 7;
    call.arguments[6] = frameB + 156;
    call.arguments[7] = 7;
    call.r30          = reservation;
    call.r31          = device;
    std::uint64_t r3  = 0xFEED000000000044ULL;
    UNSCOPED_INFO("the stub returns what the SDK export returns");
    CHECK((ReplaceVideoScalerCall(call, r3) && r3 == sdk.r3.u64));
    bool same = true;
    for (std::uint32_t i = 0; i < 200; ++i)
        same = same && word(frameB + 192 + 4 * i) == expected[i];
    UNSCOPED_INFO("the stub output equals the SDK output word for word");
    CHECK(same);
    UNSCOPED_INFO("the neighbours are untouched");
    CHECK((word(frameB + 188) == kBelow && word(frameB + 992) == kAbove && word(reservation) == kHeader && word(reservation + 804) == kTail));
    auto counts = take();
    UNSCOPED_INFO("consumed through the production adapter");
    CHECK((counts.counts.consumed == 1 && counts.refusals() == 0));

    // Production refusals store nothing.
    const auto refuses = [&](VideoScalerCall bad, R reason, const char* what)
    {
        for (std::uint32_t i = 0; i < 200; ++i)
            put(frameB + 192 + 4 * i, kOutputPoison);
        std::uint64_t value = 0x77;
        UNSCOPED_INFO(std::string(what) + ": not replaced");
        CHECK((!ReplaceVideoScalerCall(bad, value) && value == 0x77));
        bool poisoned = true;
        for (std::uint32_t i = 0; i < 200; ++i)
            poisoned = poisoned && word(frameB + 192 + 4 * i) == kOutputPoison;
        UNSCOPED_INFO(std::string(what) + ": no store");
        CHECK(poisoned);
        const auto c = take();
        UNSCOPED_INFO(std::string(what) + ": refusal reason");
        CHECK((c.refusal(reason) == 1 && c.refusals() == 1));
        // Close the refused call's record, as its missing observations never come.
        ObserveVideoScalerCopy(static_cast<std::uint32_t>(bad.r1), static_cast<std::uint32_t>(bad.r30), static_cast<std::uint32_t>(bad.r31));
        (void)take();
    };
    auto bad = call;
    bad.r30  = 0x10;
    refuses(bad, R::Reservation, "reservation outside memory");
    bad     = call;
    bad.r30 = device + 0x100;
    refuses(bad, R::Alias, "reservation inside the device");
    bad    = call;
    bad.r1 = frameB + 8;
    refuses(bad, R::Stack, "misaligned stack");
    bad              = call;
    bad.arguments[6] = frameB + 160;
    refuses(bad, R::Shape, "vertical parameters");
    // The records of the refused calls are closed by the next call on this thread.
    UNSCOPED_INFO("a clean call after the refusals");
    CHECK(ReplaceVideoScalerCall(call, r3));
    ObserveVideoScalerReturn(frameB, r3, reservation, device);
    ObserveVideoScalerCopy(frameB, reservation, device);
    (void)take();
}

// Adapter maps 4 KiB and 64 KiB views by physical identity.
void testRuntimePhysicalAlias()
{
    // Device's 4 KB (4096 bytes) view aliases the reservation's 64 KB view.
    rex::Runtime runtime(std::filesystem::path{});
    UNSCOPED_INFO("runtime setup");
    CHECK(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == X_STATUS_SUCCESS);
    auto* memory = runtime.memory();
    UNSCOPED_INFO("guest memory");
    CHECK(memory != nullptr);
    if (!memory)
        return;
    const auto word = [&](std::uint32_t address) -> std::uint32_t
    {
        std::uint32_t raw = 0;
        std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), 4);
        return rex::byte_swap(raw);
    };
    const auto put = [&](std::uint32_t address, std::uint32_t value)
    {
        const auto raw = rex::byte_swap(value);
        std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), &raw, 4);
    };
    const std::uint32_t frame  = memory->SystemHeapAlloc(0x1000, 0x1000, rex::memory::kSystemHeapVirtual);
    const std::uint32_t device = memory->SystemHeapAlloc(0x6000, 0x1000, rex::memory::kSystemHeapPhysical);
    const std::uint32_t other  = memory->SystemHeapAlloc(0x2000, 0x1000, rex::memory::kSystemHeapPhysical);
    UNSCOPED_INFO("guest allocations");
    CHECK((frame && device && other));
    if (!(frame && device && other))
        return;
    // The 64 KB view at 0xA0000000 of the 64 KB block holding the device and the second allocation.
    const std::uint32_t devicePhysical = memory->GetPhysicalAddress(device), otherPhysical = memory->GetPhysicalAddress(other);
    const std::uint32_t block = devicePhysical & ~0xFFFFU;
    UNSCOPED_INFO("both allocations share one 64 KB block");
    CHECK((block == (otherPhysical & ~0xFFFFU) && otherPhysical + 0x2000 <= block + 0x10000 && devicePhysical + 0x6000 <= block + 0x10000));
    auto* view = memory->LookupHeap(0xA0000000);
    UNSCOPED_INFO("the 64 KB view of the block");
    CHECK((view && view->AllocFixed(0xA0000000 + block, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    const std::uint32_t deviceView = 0xA0000000 + devicePhysical, otherView = 0xA0000000 + otherPhysical;
    UNSCOPED_INFO("a second virtual address for the device pages");
    CHECK((memory->GetPhysicalAddress(deviceView) == devicePhysical && deviceView != device));
    put(deviceView + 0x400, 0x12345678);
    UNSCOPED_INFO("the two virtual addresses are one physical allocation");
    CHECK(word(device + 0x400) == 0x12345678);

    const auto fill = [&](std::uint32_t reservation)
    {
        for (std::uint32_t offset = 0; offset < 2464; offset += 4)
            put(frame + offset, 0x5A000000 | offset);
        for (std::uint32_t i = 0; i < 200; ++i)
            put(frame + 192 + 4 * i, kOutputPoison);
        put(frame + 188, kBelow);
        put(frame + 992, kAbove);
        put(frame + 84, frame + 172);
        put(frame + 92, frame + 992);
        put(frame + 100, frame + 192);
        put(frame + 108, 200);
        put(reservation, kHeader);
        for (std::uint32_t i = 0; i < 200; ++i)
            put(reservation + 4 + 4 * i, kCopyPoison);
        put(reservation + 804, kTail);
    };
    const auto makeCall = [&](std::uint32_t reservation)
    {
        VideoScalerCall call;
        call.r1           = frame;
        call.arguments[6] = frame + 156;
        call.r30          = reservation;
        call.r31          = device;
        return call;
    };
    SetVideoScalerOperationsForTesting(nullptr);
    Cvars cvars(true, false, true, device);

    // The reservation sits inside the device allocation, reached through the other view.
    const std::uint32_t overlapping = deviceView + 0x400;
    fill(overlapping);
    std::uint64_t r3 = 0x77;
    UNSCOPED_INFO("a physically overlapping copy destination is refused");
    CHECK((!ReplaceVideoScalerCall(makeCall(overlapping), r3) && r3 == 0x77));
    bool poisoned = true;
    for (std::uint32_t i = 0; i < 200; ++i)
        poisoned = poisoned && word(frame + 192 + 4 * i) == kOutputPoison;
    UNSCOPED_INFO("the refusal stored nothing");
    CHECK(poisoned);
    auto counts = take();
    UNSCOPED_INFO("counted as alias");
    CHECK((counts.refusal(R::Alias) == 1 && counts.refusals() == 1 && counts.counts.consumed == 0));
    ObserveVideoScalerCopy(frame, overlapping, device);
    (void)take();

    // The same machinery with a reservation on other pages is admitted.
    const std::uint32_t separate = otherView + 0x10;
    fill(separate);
    UNSCOPED_INFO("a disjoint reservation through the same view is consumed");
    CHECK((ReplaceVideoScalerCall(makeCall(separate), r3) && r3 == 200));
    ObserveVideoScalerCopy(frame, separate, device);
    counts = take();
    UNSCOPED_INFO("consumed, no refusal");
    CHECK((counts.counts.consumed == 1 && counts.refusals() == 0));
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 15>;

const CaseTable kCases{
    CaseTable::value_type{ "testConstants", testConstants },
    CaseTable::value_type{ "testDeriveAndApply", testDeriveAndApply },
    CaseTable::value_type{ "testSlotDecoding", testSlotDecoding },
    CaseTable::value_type{ "testShapeRefusals", testShapeRefusals },
    CaseTable::value_type{ "testRefusalTable", testRefusalTable },
    CaseTable::value_type{ "testWriteFault", testWriteFault },
    CaseTable::value_type{ "testServicesAndLease", testServicesAndLease },
    CaseTable::value_type{ "testDisabledWork", testDisabledWork },
    CaseTable::value_type{ "testExitCheck", testExitCheck },
    CaseTable::value_type{ "testObservationPairing", testObservationPairing },
    CaseTable::value_type{ "testContinuation", testContinuation },
    CaseTable::value_type{ "testHookAbi", testHookAbi },
    CaseTable::value_type{ "testReport", testReport },
    CaseTable::value_type{ "testRuntimeParity", testRuntimeParity },
    CaseTable::value_type{ "testRuntimePhysicalAlias", testRuntimePhysicalAlias }
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

TEST_CASE("video_scaler_stub", "[ctest][video-scaler-stub][timeout-120]")
{
    runCases({});
}

// Runs one named case:
//   video-scaler-stub.case <testName>
TEST_CASE("video_scaler_stub_selected", "[.][manual][video-scaler-stub]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(videoScalerStubCase, "video-scaler-stub.case", "video_scaler_stub_selected")
