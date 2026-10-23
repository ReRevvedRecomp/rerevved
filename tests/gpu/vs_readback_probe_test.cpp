#include "gpu/guest_d3d/vs_readback_probe.h"
#include "manual_cases.hpp"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, native_vs_readback_probe);

// The production wrappers, compiled from vs_readback_probe_hooks.cpp.
REX_EXTERN(sub_82662D28);
REX_EXTERN(sub_82694480);

namespace rerevved::gpu::guest_d3d
{

// Lets a test hold the recorder lock the way a terminated guest writer would.
struct VsReadbackProbeTestAccess
{
    static std::timed_mutex& RecorderLock(VsReadbackProbe& probe)
    {
        return probe.mutex_;
    }
};

} // namespace rerevved::gpu::guest_d3d

namespace
{

using namespace rerevved::gpu::guest_d3d;

// Fixture layout. Q is the refresh record, O its owner, VS the shader resource.
constexpr std::uint32_t kQ = 0x10000, kO = 0x11000, kVs = 0x20000, kSp = 0x30000, kCode = 0x40000, kDest = 0x50000, kBuffer = 0x60000, kNewBuffer = 0x70000;
constexpr std::uint32_t kDevice = 0x12340000, kFlags = 0x80000010, kThread = 0x7000;
// Deliberately asymmetric so a little-endian or swapped decode cannot match.
constexpr std::uint32_t kM = 0x00000124, kS = 0x00001038, kSum = 0x0000115C;
constexpr std::uint64_t kUpper = 0xFEDCBA9800000000ULL;
constexpr std::size_t   kArena = 0x80000;

struct Boom : std::runtime_error
{
    Boom()
    : std::runtime_error("original failed")
    {
    }
};

// Guest memory. The observer sees only Read. The fake Originals write through
// Store, which updates the expected shadow; any other change is an observer
// store and breaks the bytes == shadow checkpoints.
struct Arena
{
    std::vector<std::uint8_t>                            bytes  = std::vector<std::uint8_t>(kArena, 0);
    std::vector<std::uint8_t>                            shadow = std::vector<std::uint8_t>(kArena, 0);
    std::vector<std::pair<std::uint32_t, std::size_t>>   reads;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> blocked, forbidden;
    bool                                                 throwing = false, allBlocked = false;
    unsigned                                             forbiddenReads = 0;

    static bool Read(void* context, std::uint32_t address, std::uint8_t* out, std::size_t count)
    {
        auto& memory = *static_cast<Arena*>(context);
        memory.reads.emplace_back(address, count);
        if (memory.throwing)
            throw std::logic_error("reader failed");
        for (const auto& [begin, end] : memory.forbidden)
            if (address < end && begin < address + count)
                ++memory.forbiddenReads;
        if (memory.allBlocked || address >= memory.bytes.size() || count > memory.bytes.size() - address)
            return false;
        for (const auto& [begin, end] : memory.blocked)
            if (address < end && begin < address + count)
                return false;
        std::memcpy(out, memory.bytes.data() + address, count);
        return true;
    }

    std::uint32_t load(std::uint32_t address) const
    {
        return (std::uint32_t(bytes.at(address)) << 24) | (std::uint32_t(bytes.at(address + 1)) << 16) | (std::uint32_t(bytes.at(address + 2)) << 8) | bytes.at(address + 3);
    }

    // Original-side or setup store: both views change together.
    void store(std::uint32_t address, std::uint32_t value)
    {
        for (unsigned i = 0; i < 4; ++i)
            bytes.at(address + i) = shadow.at(address + i) = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }

    void fill(std::uint32_t address, std::uint32_t count, std::uint8_t seed)
    {
        for (std::uint32_t i = 0; i < count; ++i)
            bytes.at(address + i) = shadow.at(address + i) = static_cast<std::uint8_t>(seed + i * 7);
    }

    bool unchanged() const
    {
        return bytes == shadow;
    }

    void setup()
    {
        store(kQ + 4, kVs);
        store(kQ + 8, kBuffer);
        store(kQ + 0x14, kFlags);
        store(kO + 0x0C, kDevice);
        // The metadata image contains the M and S words themselves.
        fill(kVs + 0x368, kM, 0x11);
        fill(kCode, kS, 0x33);
        store(kVs + 0x20, kCode);
        store(kVs + 0x36C, kM);
        store(kVs + 0x370, kS);
        store(kSp + 84, 0xA5A5A5A5);
    }
};

Arena* arena = nullptr;

struct Behavior
{
    bool     children = true, throwQuery = false, throwCopy = false, throwRefresh = false, catchQueryThrow = false;
    unsigned refreshCalls = 0, readbackCalls = 0;
    // Output pointer of the child copy; a size pointer other than the stack slot.
    std::uint32_t destination = kDest, copySizePointer = 0;
    std::uint64_t result = 0xFEED000080004005ULL;
};

Behavior* behavior = nullptr;

void checkpoint(std::string_view where)
{
    UNSCOPED_INFO(where);
    CHECK(arena->unchanged());
}

// Models 0x82694480: a query stores M+S at r5; a copy writes metadata then
// code at r4. Registers and result are clobbered on purpose.
void fakeReadback(PPCContext& ctx, std::uint8_t*)
{
    ++behavior->readbackCalls;
    checkpoint("no observer store before readback Original");
    const auto vs = ctx.r3.u32, destination = ctx.r4.u32, sizePointer = ctx.r5.u32;
    const auto m = arena->load(vs + 0x36C), s = arena->load(vs + 0x370);
    if (destination == 0 ? behavior->throwQuery : behavior->throwCopy)
        throw Boom();
    if (destination == 0)
        arena->store(sizePointer, m + s);
    else if (destination + std::uint64_t{ m } + s <= kArena)
    {
        std::vector<std::uint8_t> staged(arena->bytes.begin() + vs + 0x368, arena->bytes.begin() + vs + 0x368 + m);
        const auto                code = arena->load(vs + 0x20);
        staged.insert(staged.end(), arena->bytes.begin() + code, arena->bytes.begin() + code + s);
        for (std::size_t i = 0; i < staged.size(); ++i)
            arena->bytes[destination + i] = arena->shadow[destination + i] = staged[i];
    }
    ctx.r3.u64 = 0x1BCDEF0100000000ULL | (destination ? 7U : 0U);
    ctx.r4.u64 = 0x1122334455667788ULL;
    ctx.r5.u64 = 0x99AABBCCDDEEFF00ULL;
    ctx.r1.u64 = 0x0102030405060708ULL;
    ctx.lr     = 0x7FFFFFFF12345678ULL;
    checkpoint("no observer store after readback Original");
}

// Models 0x82662D28: clear Qdirty, optionally query and copy through the real
// wrappers, replace the buffer and return with changed registers.
void fakeRefresh(PPCContext& ctx, std::uint8_t* base)
{
    ++behavior->refreshCalls;
    checkpoint("no observer store before refresh Original");
    const auto q = ctx.r4.u32, sp = ctx.r1.u32;
    arena->store(q + 0x14, arena->load(q + 0x14) & 0x7FFFFFFFU);
    if (behavior->children)
    {
        const auto vs = arena->load(q + 4);
        ctx.r1.u64    = sp - 144;
        ctx.r3.u64    = kUpper | vs;
        ctx.r4.u64    = 0;
        ctx.r5.u64    = ctx.r1.u32 + 84;
        ctx.lr        = kVsReadbackQueryLr;
        try
        {
            sub_82694480(ctx, base);
        }
        catch (const Boom&)
        {
            if (!behavior->catchQueryThrow)
                throw;
        }
        ctx.r1.u64 = sp - 144;
        ctx.r3.u64 = kUpper | vs;
        ctx.r4.u64 = kUpper | behavior->destination;
        ctx.r5.u64 = behavior->copySizePointer ? behavior->copySizePointer : ctx.r1.u32 + 84;
        ctx.lr     = kVsReadbackCopyLr;
        sub_82694480(ctx, base);
        ctx.r1.u64 = sp;
    }
    if (behavior->throwRefresh)
        throw Boom();
    arena->store(q + 8, kNewBuffer);
    ctx.r3.u64 = behavior->result;
    ctx.r4.u64 = 0xDEADBEEF12345678ULL;
    ctx.r5.u64 = 0x0F0F0F0F0F0F0F0FULL;
    ctx.lr     = 0xAAAAAAAA55555555ULL;
    checkpoint("no observer store after refresh Original");
}

} // namespace

extern "C" REX_FUNC(__imp__sub_82662D28)
{
    fakeRefresh(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82694480)
{
    fakeReadback(ctx, base);
}

namespace
{

PPCContext makeContext(std::uint32_t thread, std::uint32_t sp, std::uint64_t lr, std::uint32_t r3, std::uint32_t r4, std::uint32_t r5)
{
    PPCContext context{};
    std::memset(&context, 0x39, sizeof(context));
    context.r13.u64 = (0x5555ULL << 32) | thread;
    context.r1.u64  = (0x1122ULL << 32) | sp;
    context.lr      = lr;
    context.r3.u64  = kUpper | r3;
    context.r4.u64  = (0x0BADF00DULL << 32) | r4;
    context.r5.u64  = (0x0DDBA11ULL << 32) | r5;
    return context;
}

enum class Entry
{
    Refresh,
    Readback
};

struct Outcome
{
    PPCContext                context{};
    unsigned                  originals = 0;
    bool                      threw = false, unchanged = false;
    std::string               what;
    std::vector<std::uint8_t> memory;
};

void enable(bool on)
{
    vsReadbackProbeState.store(on ? 1 : 0, std::memory_order_relaxed);
}

Outcome invoke(Entry entry, PPCContext context, Arena& memory, Behavior& model, bool enabled)
{
    arena    = &memory;
    behavior = &model;
    enable(enabled);
    Outcome out;
    try
    {
        if (entry == Entry::Refresh)
            sub_82662D28(context, nullptr);
        else
            sub_82694480(context, nullptr);
    }
    catch (const std::exception& error)
    {
        out.threw = true;
        out.what  = error.what();
    }
    enable(false);
    std::memcpy(&out.context, &context, sizeof(context));
    out.originals = entry == Entry::Refresh ? model.refreshCalls : model.readbackCalls;
    out.unchanged = memory.unchanged();
    out.memory    = memory.bytes;
    return out;
}

bool sameOutcome(const Outcome& a, const Outcome& b)
{
    return std::memcmp(&a.context, &b.context, sizeof(PPCContext)) == 0 && a.memory == b.memory && a.originals == b.originals && a.threw == b.threw && a.what == b.what;
}

PPCContext refreshContext(std::uint64_t lr = kVsReadbackRefreshPrepLr)
{
    return makeContext(kThread, kSp, lr, kO, kQ, 0x12345);
}

PPCContext queryContext(std::uint32_t sizePointer = kSp + 84, std::uint64_t lr = kVsReadbackQueryLr)
{
    return makeContext(kThread, kSp, lr, kVs, 0, sizePointer);
}

PPCContext copyContext(std::uint32_t sizePointer = kSp + 84, std::uint32_t destination = kDest)
{
    return makeContext(kThread, kSp, kVsReadbackCopyLr, kVs, destination, sizePointer);
}

VsReadbackReport drain()
{
    return TakeVsReadbackReport();
}

using Totals = std::array<std::uint64_t, kVsReadbackKinds>;

bool conserved(const VsReadbackReport& report)
{
    for (std::size_t k = 0; k < kVsReadbackKinds; ++k)
        if (report.enterTotal[k] != report.returnTotal[k] + report.abandonTotal[k] + report.pending[k])
            return false;
    return true;
}

constexpr std::size_t kRefresh = 0, kQuery = 1, kCopy = 2;

const VsReadbackDetail* detailOf(const VsReadbackReport& report, VsReadbackKind kind, std::size_t nth = 0)
{
    for (std::size_t i = 0; i < report.detailCount; ++i)
        if (report.details[i].kind == kind && nth-- == 0)
            return &report.details[i];
    return nullptr;
}

using Reads = std::vector<std::pair<std::uint32_t, std::size_t>>;

// ---------------------------------------------------------------------------

void testCvarDefault()
{
    UNSCOPED_INFO("cvar defaults false");
    CHECK(!REXCVAR_GET(native_vs_readback_probe));
    vsReadbackProbeState.store(-1);
    UNSCOPED_INFO("unset cache refreshes to the default false");
    CHECK(!VsReadbackProbeRequested());
    REXCVAR_SET(native_vs_readback_probe, true);
    UNSCOPED_INFO("cached flag does not become runtime mutable");
    CHECK(!VsReadbackProbeRequested());
    UNSCOPED_INFO("test refresh enables observation");
    CHECK((RefreshVsReadbackProbeRequested() && VsReadbackProbeRequested()));
    REXCVAR_SET(native_vs_readback_probe, false);
    UNSCOPED_INFO("refresh restores disabled");
    CHECK((!RefreshVsReadbackProbeRequested() && !VsReadbackProbeRequested()));
    UNSCOPED_INFO("disabled reporting has no fields and does no work");
    CHECK((ReportVsReadbackProbe().empty() && ReportVsReadbackProbe(true).empty()));
}

struct CountingReader
{
    unsigned calls = 0;

    static bool Read(void* context, std::uint32_t, std::uint8_t*, std::size_t)
    {
        ++static_cast<CountingReader*>(context)->calls;
        throw std::logic_error("disabled path must not read");
    }
};

void testDisabledPath()
{
    drain();
    CountingReader         reader;
    const VsReadbackAccess access{ CountingReader::Read, &reader };
    SetVsReadbackAccess(&access);
    for (const auto entry : { Entry::Refresh, Entry::Readback })
    {
        Arena    memory;
        Behavior model;
        memory.setup();
        const auto outcome = invoke(entry, entry == Entry::Refresh ? refreshContext() : copyContext(), memory, model, false);
        UNSCOPED_INFO("disabled wrapper runs Original exactly once with no observer store");
        CHECK((outcome.originals == 1 && !outcome.threw && outcome.unchanged));
    }
    SetVsReadbackAccess(nullptr);
    const auto report = drain();
    UNSCOPED_INFO("disabled wrappers never call the reader");
    CHECK(reader.calls == 0);
    Totals zero{};
    UNSCOPED_INFO("disabled wrappers record nothing");
    CHECK((report.enterTotal == zero && report.returnTotal == zero && report.abandonTotal == zero && report.pending == zero && report.detailCount == 0));
    UNSCOPED_INFO("disabled wrappers increment no counter");
    CHECK((report.counts.probeBytes == 0 && report.counts.unreadable == 0 && report.counts.pairError == 0));
    // Enabling the same fixture does read, so the check above distinguishes the paths.
    SetVsReadbackAccess(&access);
    Arena    memory;
    Behavior model;
    memory.setup();
    invoke(Entry::Readback, copyContext(), memory, model, true);
    SetVsReadbackAccess(nullptr);
    UNSCOPED_INFO("enabled wrapper reads through the installed reader");
    CHECK(reader.calls == 3);
    drain();
}

// ---------------------------------------------------------------------------

struct Condition
{
    const char* name;
    void (*prepare)(Arena&, Behavior&);
    // Whether the observer's reader fails, and whether an interval is invalid,
    // for the refresh entry and for the readback entry.
    std::array<bool, 2> unreadable, range;
};

void testWrapperParity()
{
    const std::array<Condition, 8> conditions{ {
        { "readable", [](Arena&, Behavior&)
          {
          },
          { false, false },
          { false, false } },
        { "all unreadable", [](Arena& memory, Behavior&)
          {
              memory.allBlocked = true;
          },
          { true, true },
          { false, false } },
        { "throwing reader", [](Arena& memory, Behavior&)
          {
              memory.throwing = true;
          },
          { true, true },
          { false, false } },
        { "partial unreadable", [](Arena& memory, Behavior&)
          {
              memory.blocked.emplace_back(kVs + 0x370, kVs + 0x374);
          },
          { true, true },
          { false, false } },
        { "null shader", [](Arena& memory, Behavior&)
          {
              memory.store(kQ + 4, 0);
          },
          { true, false },
          { false, false } },
        { "huge extents", [](Arena& memory, Behavior&)
          {
              memory.store(kVs + 0x36C, 0x80000000U);
              memory.store(kVs + 0x370, 0x80000010U);
          },
          { false, false },
          { true, true } },
        { "no children", [](Arena&, Behavior& model)
          {
              model.children = false;
          },
          { false, false },
          { false, false } },
        { "unreadable query slot", [](Arena& memory, Behavior&)
          {
              memory.blocked.emplace_back(kSp - 60, kSp - 56);
          },
          { true, false },
          { false, false } },
    } };
    for (const auto& condition : conditions)
        for (const auto entry : { Entry::Refresh, Entry::Readback })
        {
            Arena    offMemory, onMemory;
            Behavior offModel, onModel;
            offMemory.setup();
            onMemory.setup();
            condition.prepare(offMemory, offModel);
            condition.prepare(onMemory, onModel);
            const auto context = entry == Entry::Refresh ? refreshContext() : copyContext();
            const auto off     = invoke(entry, context, offMemory, offModel, false);
            drain();
            // The observer reads through the arena's reader, which applies the condition;
            // the Original reads the arena directly and never sees it.
            const VsReadbackAccess access{ Arena::Read, &onMemory };
            SetVsReadbackAccess(&access);
            const auto on = invoke(entry, context, onMemory, onModel, true);
            SetVsReadbackAccess(nullptr);
            const auto report = drain();
            const auto which  = entry == Entry::Refresh ? 0U : 1U;
            UNSCOPED_INFO(condition.name);
            CHECK((off.originals == 1 && on.originals == 1));
            UNSCOPED_INFO(condition.name);
            CHECK(sameOutcome(off, on));
            UNSCOPED_INFO(condition.name);
            CHECK((off.unchanged && on.unchanged));
            UNSCOPED_INFO("the observer read through the installed reader and the baseline did not");
            CHECK((!onMemory.reads.empty() && offMemory.reads.empty()));
            UNSCOPED_INFO(condition.name);
            CHECK((report.counts.unreadable != 0) == condition.unreadable[which]);
            UNSCOPED_INFO(condition.name);
            CHECK((report.counts.rangeOverflow != 0) == condition.range[which]);
            UNSCOPED_INFO(condition.name);
            CHECK((report.counts.pairError == 0 && conserved(report)));
        }
}

void testRegisterParity()
{
    for (const auto entry : { Entry::Refresh, Entry::Readback })
    {
        Arena    offMemory, onMemory;
        Behavior offModel, onModel;
        offMemory.setup();
        onMemory.setup();
        const auto context = entry == Entry::Refresh ? refreshContext() : queryContext();
        const auto off     = invoke(entry, context, offMemory, offModel, false);
        const auto on      = invoke(entry, context, onMemory, onModel, true);
        UNSCOPED_INFO("enabled context and memory match the disabled baseline byte for byte");
        CHECK(sameOutcome(off, on));
        // Original's own effects are visible, so the comparison is not vacuous.
        UNSCOPED_INFO("baseline Original changes lr, r4 and r5");
        CHECK((off.context.lr != context.lr && off.context.r4.u64 != context.r4.u64 && off.context.r5.u64 != context.r5.u64));
        UNSCOPED_INFO("baseline Original returns a result with nonzero upper bits");
        CHECK((off.context.r3.u64 >> 32 != 0 || entry == Entry::Refresh));
        UNSCOPED_INFO("refresh result keeps its full 64 bits");
        CHECK((entry != Entry::Refresh || off.context.r3.u64 == 0xFEED000080004005ULL));
        UNSCOPED_INFO("baseline Original changes the stack pointer");
        CHECK((entry == Entry::Refresh || off.context.r1.u64 == 0x0102030405060708ULL));
        drain();
    }
}

// ---------------------------------------------------------------------------

void testRefreshRoundTrip()
{
    drain();
    Arena    memory;
    Behavior model;
    memory.setup();
    VsReadbackAccess access{ Arena::Read, &memory };
    SetVsReadbackAccess(&access);
    const auto outcome = invoke(Entry::Refresh, refreshContext(), memory, model, true);
    SetVsReadbackAccess(nullptr);
    UNSCOPED_INFO("refresh runs one query and one copy Original");
    CHECK((outcome.originals == 1 && model.readbackCalls == 2));
    const auto  report = drain();
    const auto& c      = report.counts;
    UNSCOPED_INFO("one refresh, one query and one copy");
    CHECK((c.enter[kRefresh] == 1 && c.enter[kQuery] == 1 && c.enter[kCopy] == 1));
    UNSCOPED_INFO("all three return normally");
    CHECK((c.returned[kRefresh] == 1 && c.returned[kQuery] == 1 && c.returned[kCopy] == 1));
    UNSCOPED_INFO("nothing abandoned or pending");
    CHECK((c.abandon[kRefresh] + c.abandon[kQuery] + c.abandon[kCopy] == 0 && report.pending == Totals{}));
    UNSCOPED_INFO("requested bytes are M and S exactly once");
    CHECK((c.requestedMetaBytes == 0x124 && c.requestedCodeBytes == 0x1038));
    UNSCOPED_INFO("both children are in_refresh");
    CHECK((c.queryParent[0] == 1 && c.copyParent[0] == 1 && c.queryParent[2] + c.copyParent[2] == 0));
    UNSCOPED_INFO("query size and copy header are valid once");
    CHECK((c.querySizeValid == 1 && c.copyHeaderValid == 1));
    UNSCOPED_INFO("prep caller, dirty entry and negative result");
    CHECK((c.refreshCaller[0] == 1 && c.refreshQ[0] == 1 && c.refreshNegativeResult == 1));
    UNSCOPED_INFO("diagnostic bytes and quality");
    CHECK((c.probeBytes == 56 && c.unreadable == 0 && c.pairError == 0 && c.eventDrop == 0 && c.correlationDrop == 0));
    UNSCOPED_INFO("cumulative conservation");
    CHECK(conserved(report));
    Reads expected{ { kQ + 4, 4 }, { kQ + 8, 4 }, { kQ + 0x14, 4 }, { kO + 0x0C, 4 }, { kVs + 0x36C, 4 }, { kVs + 0x370, 4 }, { kSp - 60, 4 }, { kVs + 0x36C, 4 }, { kVs + 0x370, 4 }, { kVs + 0x20, 4 }, { kQ + 4, 4 }, { kQ + 8, 4 }, { kQ + 0x14, 4 }, { kO + 0x0C, 4 } };
    UNSCOPED_INFO("reader whitelist is exactly the named fields in order");
    CHECK(memory.reads == expected);
    UNSCOPED_INFO("one detail per finished call");
    CHECK(report.detailCount == 3);
    const auto* refresh = detailOf(report, VsReadbackKind::Refresh);
    const auto* query   = detailOf(report, VsReadbackKind::Query);
    const auto* copy    = detailOf(report, VsReadbackKind::Copy);
    UNSCOPED_INFO("detail kinds");
    CHECK((refresh && query && copy));
    if (!(refresh && query && copy))
        return;
    UNSCOPED_INFO("children carry the refresh token");
    CHECK((query->parentCall == refresh->call && copy->parentCall == refresh->call && query->call != copy->call));
    UNSCOPED_INFO("full64 SP and LR retained");
    CHECK((refresh->entry.sp == ((0x1122ULL << 32) | kSp) && refresh->entry.lr == kVsReadbackRefreshPrepLr));
    UNSCOPED_INFO("child frames and link registers");
    CHECK((query->entry.sp == kSp - 144 && query->entry.lr == kVsReadbackQueryLr && copy->entry.lr == kVsReadbackCopyLr));
    UNSCOPED_INFO("raw full64 results retained");
    CHECK((refresh->exit.result == 0xFEED000080004005ULL && copy->exit.result == (0x1BCDEF0100000007ULL)));
    UNSCOPED_INFO("entry sees Qdirty, exit sees the clear");
    CHECK((refresh->entry.words[2] == kFlags && refresh->exit.words[2] == (kFlags & 0x7FFFFFFFU)));
    UNSCOPED_INFO("exit sample reads the saved Q after Original replaced the buffer");
    CHECK((refresh->exit.words[1] == kNewBuffer && refresh->entry.words[1] == kBuffer));
    UNSCOPED_INFO("query exit size is M+S");
    CHECK((query->exit.words[0] == kSum && (query->exit.valid & kVsReadbackSizeWord)));
    UNSCOPED_INFO("copy extents");
    CHECK((copy->extents.headerValid && copy->extents.output.begin == kDest && copy->extents.output.length == kSum));
    UNSCOPED_INFO("metadata and code extents");
    CHECK((copy->extents.metadata.begin == kVs + 0x368 && copy->extents.metadata.length == kM && copy->extents.code.begin == kCode && copy->extents.code.length == kS));
    UNSCOPED_INFO("raw sum");
    CHECK((copy->extents.sum32 == kSum && copy->extents.sumWide == kSum));
    UNSCOPED_INFO("disjoint copy");
    CHECK((copy->extents.overlap == 0 && !copy->extents.rangeInvalid));
    const auto fields = FormatVsReadbackCounts(report);
    for (const auto text : { " rb_read_refresh_enter=1 ", " rb_read_query_enter=1 ", " rb_read_copy_enter=1 ", " rb_read_copy_pending=0 ", " rb_read_requested_meta_bytes=292 ", " rb_read_requested_code_bytes=4152 ", " rb_read_refresh_prep=1 ", " rb_read_refresh_dirty=1 ", " rb_read_query_in_refresh=1 ", " rb_read_copy_in_refresh=1 ", " rb_read_conserved=1 " })
    {
        UNSCOPED_INFO(text);
        CHECK(fields.find(text) != std::string::npos);
    }
}

void testQueryClassification()
{
    drain();
    Arena    memory;
    Behavior model;
    memory.setup();
    VsReadbackAccess access{ Arena::Read, &memory };
    SetVsReadbackAccess(&access);
    // Original clears r4 to nonzero and r5 elsewhere; the entry kind and slot stay.
    const auto outcome = invoke(Entry::Readback, queryContext(), memory, model, true);
    SetVsReadbackAccess(nullptr);
    UNSCOPED_INFO("query fixture changes r4");
    CHECK((outcome.originals == 1 && outcome.context.r4.u64 != 0));
    const auto report = drain();
    UNSCOPED_INFO("entry destination zero is a query whatever r4 returns");
    CHECK((report.counts.enter[kQuery] == 1 && report.counts.enter[kCopy] == 0));
    UNSCOPED_INFO("queries request no copy bytes");
    CHECK((report.counts.requestedMetaBytes == 0 && report.counts.requestedCodeBytes == 0 && report.counts.copyHeaderValid == 0));
    UNSCOPED_INFO("query size sampled");
    CHECK(report.counts.querySizeValid == 1);
    Reads expected{ { kVs + 0x36C, 4 }, { kVs + 0x370, 4 }, { kSp + 84, 4 } };
    UNSCOPED_INFO("query reads M, S and the saved size word only");
    CHECK(memory.reads == expected);
    const auto* query = detailOf(report, VsReadbackKind::Query);
    UNSCOPED_INFO("size comes from the saved r5, not the return r5");
    CHECK((query && query->exit.words[0] == kSum && query->entry.sizePointer == kSp + 84));
    UNSCOPED_INFO("standalone query is outside any refresh");
    CHECK((query && query->entry.valid == (kVsReadbackM | kVsReadbackS) && query->parent == VsReadbackParent::OutsideRefresh));
    // The same call from the descriptor path keeps its kind.
    memory.reads.clear();
    SetVsReadbackAccess(&access);
    invoke(Entry::Readback, queryContext(kSp + 84, 0x825E1D30), memory, model, true);
    SetVsReadbackAccess(nullptr);
    const auto other = drain();
    UNSCOPED_INFO("other caller is unpaired");
    CHECK((other.counts.enter[kQuery] == 1 && other.counts.queryParent[2] + other.counts.queryParent[1] == 1));
}

void testCopySizePointer()
{
    drain();
    Arena    memory;
    Behavior model;
    memory.setup();
    VsReadbackAccess access{ Arena::Read, &memory };
    SetVsReadbackAccess(&access);
    // The copy body never writes r5. Make it unreadable and unmapped.
    invoke(Entry::Readback, copyContext(0xFFFFFFFE), memory, model, true);
    SetVsReadbackAccess(nullptr);
    const auto report = drain();
    Reads      expected{ { kVs + 0x36C, 4 }, { kVs + 0x370, 4 }, { kVs + 0x20, 4 } };
    UNSCOPED_INFO("copy reads only M, S and C");
    CHECK(memory.reads == expected);
    UNSCOPED_INFO("unreadable size pointer is not a failed read; its wrapped interval is range quality");
    CHECK((report.counts.unreadable == 0 && report.counts.copyHeaderValid == 1 && report.counts.rangeOverflow == 1));
    const auto* copy = detailOf(report, VsReadbackKind::Copy);
    UNSCOPED_INFO("copy has no exit sample");
    CHECK((copy && copy->extents.headerValid && !copy->extents.sizeWord.valid && copy->exit.valid == 0));
    UNSCOPED_INFO("header still requested");
    CHECK((report.counts.requestedMetaBytes == kM && report.counts.requestedCodeBytes == kS));
}

void testEndianExtents()
{
    drain();
    Arena    memory;
    Behavior model;
    memory.setup();
    VsReadbackAccess access{ Arena::Read, &memory };
    SetVsReadbackAccess(&access);
    invoke(Entry::Readback, copyContext(), memory, model, true);
    invoke(Entry::Readback, queryContext(), memory, model, true);
    SetVsReadbackAccess(nullptr);
    const auto  report = drain();
    const auto* copy   = detailOf(report, VsReadbackKind::Copy);
    const auto* query  = detailOf(report, VsReadbackKind::Query);
    UNSCOPED_INFO("details");
    CHECK((copy && query));
    if (!(copy && query))
        return;
    // Literal expectations from the fixture bytes 00 00 01 24 and 00 00 10 38.
    UNSCOPED_INFO("BE32 decode of M, S and C");
    CHECK((copy->entry.words[0] == 0x124 && copy->entry.words[1] == 0x1038 && copy->entry.words[2] == 0x40000));
    UNSCOPED_INFO("metadata extent");
    CHECK((copy->extents.metadata.begin == 0x20368 && copy->extents.metadata.length == 0x124));
    UNSCOPED_INFO("code extent");
    CHECK((copy->extents.code.begin == 0x40000 && copy->extents.code.length == 0x1038));
    UNSCOPED_INFO("output extent");
    CHECK((copy->extents.output.begin == 0x50000 && copy->extents.output.length == 0x115C && copy->extents.sum32 == 0x115C));
    UNSCOPED_INFO("BE32 size sample");
    CHECK(query->exit.words[0] == 0x115C);
    UNSCOPED_INFO("requested totals");
    CHECK((report.counts.requestedMetaBytes == 0x124 && report.counts.requestedCodeBytes == 0x1038));
}

void testInvalidAndWrap()
{
    // Direct API with a local probe so every case has its own counters.
    const auto run = [](auto setup, std::uint32_t vs, std::uint32_t destination, std::uint32_t sizePointer)
    {
        Arena memory;
        memory.setup();
        setup(memory);
        VsReadbackProbe  probe;
        VsReadbackAccess access{ Arena::Read, &memory };
        auto             context = makeContext(kThread, kSp, kVsReadbackCopyLr, vs, destination, sizePointer);
        auto             token   = BeginVsReadbackProbe(probe, VsReadbackFunction::Readback, context, access);
        EndVsReadbackProbe(probe, token, context, access);
        return std::pair{ probe.Take(), memory.reads };
    };
    {
        // Boundary VS: the field addresses wrap, so no read is attempted.
        const auto [report, reads] = run([](Arena&)
                                         {
                                         },
                                         0xFFFFFFF0U,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("wrapped field addresses are never read");
        CHECK(reads.empty());
        UNSCOPED_INFO("failed reads are not coerced to zero");
        CHECK((report.counts.unreadable == 3 && report.counts.copyHeaderValid == 0 && report.counts.requestedMetaBytes + report.counts.requestedCodeBytes == 0));
        const auto* copy = detailOf(report, VsReadbackKind::Copy);
        UNSCOPED_INFO("no field is valid");
        CHECK((copy && copy->entry.valid == 0 && !copy->extents.headerComplete && copy->extents.rangeInvalid == false));
        UNSCOPED_INFO("the call still counts");
        CHECK((report.enterTotal[kCopy] == 1 && report.returnTotal[kCopy] == 1));
    }
    {
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x36C, 0x80000000U);
                                             memory.store(kVs + 0x370, 0x80000010U);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("bounded scalar-only reads");
        CHECK((reads.size() == 3 && std::all_of(reads.begin(), reads.end(), [](const auto& r)
                                                {
                                                    return r.second == 4;
                                                })));
        UNSCOPED_INFO("M+S overflow is one range event with no requested bytes");
        CHECK((report.counts.rangeOverflow == 1 && report.counts.copyHeaderValid == 0 && report.counts.requestedMetaBytes == 0 && report.counts.requestedCodeBytes == 0));
        const auto* copy = detailOf(report, VsReadbackKind::Copy);
        UNSCOPED_INFO("raw wrapped sum and wide sum are both retained");
        CHECK((copy && copy->extents.sum32 == 0x00000010 && copy->extents.sumWide == 0x100000010ULL && copy->extents.rangeInvalid && !copy->extents.headerValid && copy->extents.headerComplete));
        UNSCOPED_INFO("range quality bit");
        CHECK((copy && (copy->quality & kVsReadbackQualityRange)));
    }
    {
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x36C, 0x10);
                                             memory.store(kVs + 0x370, 0x20);
                                         },
                                         kVs,
                                         0xFFFFFFF0U,
                                         kSp + 84);
        UNSCOPED_INFO("destination end wraps the address space");
        CHECK((report.counts.rangeOverflow == 1 && report.counts.requestedMetaBytes == 0));
    }
    {
        // Valid empty extents: S zero with a null code pointer, then M zero.
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x370, 0);
                                             memory.store(kVs + 0x20, 0);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("empty code extent is valid");
        CHECK((report.counts.copyHeaderValid == 1 && report.counts.rangeOverflow == 0 && report.counts.requestedMetaBytes == kM && report.counts.requestedCodeBytes == 0));
        const auto* copy = detailOf(report, VsReadbackKind::Copy);
        UNSCOPED_INFO("empty extents intersect nothing");
        CHECK((copy && copy->extents.code.valid && copy->extents.code.length == 0 && copy->extents.overlap == 0));
    }
    {
        // An empty code extent whose pointer lies inside the output range still intersects nothing.
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x370, 0);
                                             memory.store(kVs + 0x20, kDest + 8);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        const auto* copy           = detailOf(report, VsReadbackKind::Copy);
        UNSCOPED_INFO("an empty extent inside a nonempty one is not an overlap");
        CHECK((report.counts.copyHeaderValid == 1 && report.counts.overlap == 0 && copy && copy->extents.overlap == 0));
    }
    {
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x36C, 0);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("empty metadata extent is valid");
        CHECK((report.counts.copyHeaderValid == 1 && report.counts.requestedMetaBytes == 0 && report.counts.requestedCodeBytes == kS));
    }
    {
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.store(kVs + 0x20, 0);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("nonempty code at a null pointer is invalid");
        CHECK((report.counts.rangeOverflow == 1 && report.counts.copyHeaderValid == 0));
    }
    {
        const auto [report, reads] = run([](Arena& memory)
                                         {
                                             memory.blocked.emplace_back(kVs + 0x370, kVs + 0x374);
                                         },
                                         kVs,
                                         kDest,
                                         kSp + 84);
        UNSCOPED_INFO("an unreadable field is unreadable, not a range event");
        CHECK((report.counts.unreadable == 1 && report.counts.rangeOverflow == 0 && report.counts.copyHeaderValid == 0 && report.counts.requestedCodeBytes == 0));
        const auto* copy = detailOf(report, VsReadbackKind::Copy);
        UNSCOPED_INFO("validity bits are per field");
        CHECK((copy && copy->entry.valid == (kVsReadbackM | kVsReadbackC) && (copy->quality & kVsReadbackQualityUnreadable)));
    }
}

void testQdirtyAndCallers()
{
    drain();

    struct Case
    {
        std::uint64_t lr;
        std::uint32_t flags;
        bool          flagsBlocked;
        std::size_t   caller, state;
    };

    const std::array<Case, 6> cases{ {
        { kVsReadbackRefreshPrepLr, 0x80000010, false, 0, 0 },
        { kVsReadbackRefreshDescriptorLr, 0x00000010, false, 1, 1 },
        { 0x8265E270ULL | (1ULL << 32), 0x80000000, false, 2, 0 },
        { 0x82662D64, 0x00000000, true, 2, 2 },
        { kVsReadbackRefreshPrepLr, 0xFFFFFFFF, false, 0, 0 },
        { 0, 0x7FFFFFFF, false, 2, 1 },
    } };
    for (const auto& item : cases)
    {
        Arena    memory;
        Behavior model;
        memory.setup();
        model.children = false;
        memory.store(kQ + 0x14, item.flags);
        if (item.flagsBlocked)
            memory.blocked.emplace_back(kQ + 0x14, kQ + 0x18);
        VsReadbackAccess access{ Arena::Read, &memory };
        SetVsReadbackAccess(&access);
        invoke(Entry::Refresh, refreshContext(item.lr), memory, model, true);
        SetVsReadbackAccess(nullptr);
        const auto report = drain();
        UNSCOPED_INFO("caller buckets partition by full64 LR");
        CHECK((report.counts.refreshCaller[item.caller] == 1 && report.counts.refreshCaller[0] + report.counts.refreshCaller[1] + report.counts.refreshCaller[2] == 1));
        UNSCOPED_INFO("entry Qdirty buckets partition");
        CHECK((report.counts.refreshQ[item.state] == 1 && report.counts.refreshQ[0] + report.counts.refreshQ[1] + report.counts.refreshQ[2] == 1));
        const auto* refresh = detailOf(report, VsReadbackKind::Refresh);
        if (!refresh)
            continue;
        if (!item.flagsBlocked)
        {
            UNSCOPED_INFO("entry flags are read before Original clears Qdirty");
            CHECK((refresh->entry.words[2] & kVsReadbackQDirty) == (item.flags & kVsReadbackQDirty));
            UNSCOPED_INFO("exit flags are read from the saved Q and show the clear");
            CHECK(((refresh->exit.valid & kVsReadbackQFlags) && (refresh->exit.words[2] & kVsReadbackQDirty) == 0));
            UNSCOPED_INFO("exit reads the saved Q even though Original returned r4 elsewhere");
            CHECK(refresh->exit.words[1] == kNewBuffer);
        }
        else
        {
            UNSCOPED_INFO("unreadable flags stay unknown");
            CHECK((!(refresh->entry.valid & kVsReadbackQFlags) && !(refresh->exit.valid & kVsReadbackQFlags)));
        }
    }
}

// ---------------------------------------------------------------------------

struct Pair
{
    PPCContext context;
};

VsReadbackToken beginRefresh(VsReadbackProbe& probe, Arena& memory, std::uint32_t thread, std::uint32_t sp, std::uint32_t q, PPCContext& context)
{
    VsReadbackAccess access{ Arena::Read, &memory };
    context = makeContext(thread, sp, kVsReadbackRefreshPrepLr, kO, q, 0);
    return BeginVsReadbackProbe(probe, VsReadbackFunction::Refresh, context, access);
}

VsReadbackToken beginChild(VsReadbackProbe& probe, Arena& memory, std::uint32_t thread, std::uint32_t sp, bool query, std::uint32_t vs, std::uint64_t lr, PPCContext& context)
{
    VsReadbackAccess access{ Arena::Read, &memory };
    context = makeContext(thread, sp, lr, vs, query ? 0 : kDest, kSp + 84);
    return BeginVsReadbackProbe(probe, VsReadbackFunction::Readback, context, access);
}

void endCall(VsReadbackProbe& probe, Arena& memory, VsReadbackToken& token, PPCContext& context)
{
    VsReadbackAccess access{ Arena::Read, &memory };
    EndVsReadbackProbe(probe, token, context, access);
}

// A second shader record so mismatches have something to mismatch against.
constexpr std::uint32_t kQ2 = 0x14000, kVs2 = 0x24000;

void setupSecond(Arena& memory)
{
    memory.store(kQ2 + 4, kVs2);
    memory.store(kQ2 + 8, kBuffer);
    memory.store(kQ2 + 0x14, kFlags);
    memory.store(kVs2 + 0x20, kCode);
    memory.store(kVs2 + 0x36C, kM);
    memory.store(kVs2 + 0x370, kS);
}

void testCorrelation()
{
    Arena memory;
    memory.setup();
    setupSecond(memory);
    VsReadbackProbe probe;
    PPCContext      rc, qc, cc, rc2, other;
    // Same-pointer refreshes on two guest threads keep their own parent.
    auto a  = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    auto b  = beginRefresh(probe, memory, 0x7001, 0x38000, kQ, rc2);
    auto qa = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackQueryLr, qc);
    auto qb = beginChild(probe, memory, 0x7001, 0x37F70, true, kVs, kVsReadbackQueryLr, cc);
    UNSCOPED_INFO("thread A child pairs to A");
    CHECK((qa.parent == VsReadbackParent::InRefresh && qa.parentCall == a.call));
    UNSCOPED_INFO("thread B child pairs to B");
    CHECK((qb.parent == VsReadbackParent::InRefresh && qb.parentCall == b.call && a.call != b.call));
    endCall(probe, memory, qa, qc);
    endCall(probe, memory, qb, cc);
    // A third thread with the same pointers has no refresh: outside, never borrowed.
    auto outside = beginChild(probe, memory, 0x7002, 0x2FF70, true, kVs, kVsReadbackQueryLr, other);
    UNSCOPED_INFO("no pairing by pointer alone or across threads");
    CHECK((outside.parent == VsReadbackParent::OutsideRefresh && outside.reason == VsReadbackParentReason::NoActiveRefresh && outside.parentCall == 0));
    endCall(probe, memory, outside, other);
    // Unknown LR, kind mismatch and shader mismatch pair to nothing.
    auto wrongLr = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, 0x82662D68, other);
    UNSCOPED_INFO("unknown LR is not paired");
    CHECK((wrongLr.parent == VsReadbackParent::Unknown && wrongLr.reason == VsReadbackParentReason::LrMismatch));
    endCall(probe, memory, wrongLr, other);
    auto swapped = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackCopyLr, other);
    UNSCOPED_INFO("query at the copy site is not paired");
    CHECK((swapped.parent == VsReadbackParent::Unknown && swapped.reason == VsReadbackParentReason::LrMismatch));
    endCall(probe, memory, swapped, other);
    auto wrongVs = beginChild(probe, memory, 0x7000, 0x2FF70, false, kVs2, kVsReadbackCopyLr, other);
    UNSCOPED_INFO("shader identity must agree");
    CHECK((wrongVs.parent == VsReadbackParent::Unknown && wrongVs.reason == VsReadbackParentReason::VsMismatch));
    endCall(probe, memory, wrongVs, other);
    auto notNested = beginChild(probe, memory, 0x7000, 0x31000, true, kVs, kVsReadbackQueryLr, other);
    UNSCOPED_INFO("a frame above the refresh does not nest");
    CHECK((notNested.parent == VsReadbackParent::Unknown && notNested.reason == VsReadbackParentReason::NotNested));
    endCall(probe, memory, notNested, other);
    auto noThread = beginChild(probe, memory, 0, 0x2FF70, true, kVs, kVsReadbackQueryLr, other);
    UNSCOPED_INFO("thread zero is never paired");
    CHECK((noThread.parent == VsReadbackParent::Unknown && noThread.reason == VsReadbackParentReason::ThreadUnknown));
    endCall(probe, memory, noThread, other);
    endCall(probe, memory, a, rc);
    endCall(probe, memory, b, rc2);
    // Recursion: the nearest enclosing frame owns the child.
    PPCContext inner, deep, ctx;
    auto       outer  = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    auto       nested = beginRefresh(probe, memory, 0x7000, 0x2FE00, kQ2, inner);
    auto       child  = beginChild(probe, memory, 0x7000, 0x2FD00, true, kVs2, kVsReadbackQueryLr, deep);
    UNSCOPED_INFO("recursive refresh pairs the nearest frame");
    CHECK((child.parent == VsReadbackParent::InRefresh && child.parentCall == nested.call && nested.call != outer.call));
    auto skipped = beginChild(probe, memory, 0x7000, 0x2FD00, true, kVs, kVsReadbackQueryLr, ctx);
    UNSCOPED_INFO("an invalid nearest frame is not skipped for an outer one");
    CHECK((skipped.parent == VsReadbackParent::Unknown && skipped.reason == VsReadbackParentReason::VsMismatch));
    endCall(probe, memory, skipped, ctx);
    endCall(probe, memory, child, deep);
    endCall(probe, memory, nested, inner);
    auto again = beginChild(probe, memory, 0x7000, 0x2FD00, true, kVs, kVsReadbackQueryLr, ctx);
    UNSCOPED_INFO("after the inner frame returns the outer frame is nearest");
    CHECK((again.parent == VsReadbackParent::InRefresh && again.parentCall == outer.call));
    endCall(probe, memory, again, ctx);
    endCall(probe, memory, outer, rc);
    const auto report = probe.Take();
    UNSCOPED_INFO("everything finished cleanly");
    CHECK((report.pending == Totals{} && conserved(report) && report.counts.pairError == 0));
    UNSCOPED_INFO("parent partition covers every query");
    CHECK((report.counts.queryParent[0] == 4 && report.counts.queryParent[1] == 1 && report.counts.queryParent[2] == 5 && report.counts.copyParent[2] == 1));
}

void testDrainSurvival()
{
    Arena memory;
    memory.setup();
    VsReadbackProbe probe;
    PPCContext      rc, qc;
    auto            refresh = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    const auto      first   = probe.Take();
    UNSCOPED_INFO("pending refresh survives a drain");
    CHECK((first.counts.enter[kRefresh] == 1 && first.pending[kRefresh] == 1 && first.enterTotal[kRefresh] == 1 && conserved(first)));
    UNSCOPED_INFO("the active refresh is listed as pending");
    CHECK((first.pendingRefreshCount == 1 && first.pendingRefresh[0].call == refresh.call));
    auto child = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackQueryLr, qc);
    UNSCOPED_INFO("parent identity survives Take");
    CHECK((child.parent == VsReadbackParent::InRefresh && child.parentCall == refresh.call));
    const auto second = probe.Take();
    UNSCOPED_INFO("interval counts reset while totals and pending persist");
    CHECK((second.counts.enter[kRefresh] == 0 && second.counts.enter[kQuery] == 1 && second.enterTotal[kRefresh] == 1 && second.pending[kRefresh] == 1 && second.pending[kQuery] == 1 && conserved(second)));
    endCall(probe, memory, child, qc);
    const auto third = probe.Take();
    UNSCOPED_INFO("child detail drains once");
    CHECK((third.pending[kQuery] == 0 && third.returnTotal[kQuery] == 1 && third.detailCount == 1 && conserved(third)));
    endCall(probe, memory, refresh, rc);
    const auto fourth = probe.Take();
    UNSCOPED_INFO("refresh detail drains once with no pair error");
    CHECK((fourth.pending[kRefresh] == 0 && fourth.returnTotal[kRefresh] == 1 && fourth.detailCount == 1 && fourth.counts.pairError == 0 && conserved(fourth)));
    const auto fifth = probe.Take();
    UNSCOPED_INFO("empty drain");
    CHECK((fifth.detailCount == 0 && fifth.counts.enter == std::array<std::uint64_t, kVsReadbackKinds>{} && fifth.enterTotal[kRefresh] == 1));
}

void testCapacity()
{
    Arena memory;
    memory.setup();
    {
        VsReadbackProbe probe;
        PPCContext      context;
        for (unsigned i = 0; i < kVsReadbackDetails + 1; ++i)
        {
            auto token = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, context);
            endCall(probe, memory, token, context);
        }
        const auto report = probe.Take();
        UNSCOPED_INFO("detail ring keeps its capacity and reports one drop");
        CHECK((report.detailCount == kVsReadbackDetails && report.counts.eventDrop == 1));
        UNSCOPED_INFO("a full detail ring never suppresses entry or return totals");
        CHECK((report.counts.enter[kRefresh] == 129 && report.counts.returned[kRefresh] == 129 && report.returnTotal[kRefresh] == 129 && report.pending[kRefresh] == 0 && conserved(report)));
    }
    {
        VsReadbackProbe              probe;
        std::vector<VsReadbackToken> tokens;
        std::vector<PPCContext>      contexts(kVsReadbackActive + 1);
        for (unsigned i = 0; i < kVsReadbackActive + 1; ++i)
            tokens.push_back(beginRefresh(probe, memory, 0x7000 + i, 0x30000, kQ, contexts[i]));
        UNSCOPED_INFO("the table denies the 65th correlation but counts the call");
        CHECK((tokens[kVsReadbackActive].tracked == false && tokens[kVsReadbackActive].counted));
        auto orphan = beginChild(probe, memory, 0x7000 + kVsReadbackActive, 0x2FF70, true, kVs, kVsReadbackQueryLr, contexts[0]);
        UNSCOPED_INFO("an untracked refresh makes children unknown, never borrowed");
        CHECK((orphan.parent == VsReadbackParent::Unknown && orphan.reason == VsReadbackParentReason::Untracked));
        auto sibling = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackQueryLr, contexts[1]);
        UNSCOPED_INFO("no pairing while an untracked refresh is live");
        CHECK((sibling.parent == VsReadbackParent::Unknown && sibling.reason == VsReadbackParentReason::Untracked));
        PPCContext childContext = contexts[0];
        endCall(probe, memory, orphan, childContext);
        endCall(probe, memory, sibling, contexts[1]);
        for (unsigned i = 0; i < kVsReadbackActive + 1; ++i)
        {
            auto context = makeContext(0x7000 + i, 0x30000, 0, kO, kQ, 0);
            endCall(probe, memory, tokens[i], context);
        }
        const auto report = probe.Take();
        UNSCOPED_INFO("exactly one correlation drop and no eviction of a live slot");
        CHECK((report.counts.correlationDrop == 1 && report.counts.pairError == 0));
        UNSCOPED_INFO("all refresh calls return");
        CHECK((report.enterTotal[kRefresh] == 65 && report.returnTotal[kRefresh] == 65 && report.pending[kRefresh] == 0 && report.untrackedRefresh == 0 && conserved(report)));
    }
}

void testPairErrors()
{
    Arena memory;
    memory.setup();
    VsReadbackProbe probe;
    PPCContext      rc, qc;
    auto            token = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    endCall(probe, memory, token, rc);
    endCall(probe, memory, token, rc);
    auto first = probe.Take();
    UNSCOPED_INFO("a second terminal event changes no total and is a pair error");
    CHECK((first.counts.pairError == 1 && first.returnTotal[kRefresh] == 1 && first.enterTotal[kRefresh] == 1 && conserved(first)));
    auto wrongThread = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    auto other       = makeContext(0x7001, 0x30000, 0, kO, kQ, 0);
    endCall(probe, memory, wrongThread, other);
    auto second = probe.Take();
    UNSCOPED_INFO("an end on another thread is a pair error but still balances");
    CHECK((second.counts.pairError == 1 && second.counts.returned[kRefresh] == 1 && second.pending[kRefresh] == 0 && conserved(second)));
    auto parent = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    auto child  = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackQueryLr, qc);
    endCall(probe, memory, parent, rc);
    auto third = probe.Take();
    UNSCOPED_INFO("a parent that finishes with a child pending is a pair error");
    CHECK((third.counts.pairError == 1 && third.pending[kRefresh] == 0 && third.pending[kQuery] == 1));
    endCall(probe, memory, child, qc);
    auto fourth = probe.Take();
    UNSCOPED_INFO("the orphaned child is a pair error and balances");
    CHECK((fourth.counts.pairError == 1 && fourth.pending[kQuery] == 0 && conserved(fourth)));
}

// ---------------------------------------------------------------------------

void testUnwind()
{
    const auto base = drain();
    {
        // A child query throws inside a refresh that handles it.
        Arena    memory;
        Behavior model;
        memory.setup();
        model.catchQueryThrow = true;
        model.throwQuery      = true;
        VsReadbackAccess access{ Arena::Read, &memory };
        SetVsReadbackAccess(&access);
        const auto outcome = invoke(Entry::Refresh, refreshContext(), memory, model, true);
        SetVsReadbackAccess(nullptr);
        UNSCOPED_INFO("handled child failure still returns from the refresh");
        CHECK((!outcome.threw && outcome.originals == 1));
        const auto report = drain();
        UNSCOPED_INFO("a throwing child is abandoned, never a return");
        CHECK((report.counts.enter[kQuery] == 1 && report.counts.abandon[kQuery] == 1 && report.counts.returned[kQuery] == 0));
        UNSCOPED_INFO("the enclosing refresh and later copy return");
        CHECK((report.counts.returned[kRefresh] == 1 && report.counts.abandon[kRefresh] == 0 && report.counts.returned[kCopy] == 1));
        UNSCOPED_INFO("the abandoned child is an abandon total, not a return total");
        CHECK((report.abandonTotal[kQuery] - base.abandonTotal[kQuery] == 1 && report.returnTotal[kQuery] == base.returnTotal[kQuery]));
        UNSCOPED_INFO("the parent stack stays valid");
        CHECK((report.counts.pairError == 0 && report.pending == Totals{} && conserved(report)));
        const auto* query = detailOf(report, VsReadbackKind::Query);
        UNSCOPED_INFO("abandoned detail keeps its parent and has no exit sample");
        CHECK((query && query->terminal == VsReadbackTerminal::Abandoned && query->parent == VsReadbackParent::InRefresh && query->exit.valid == 0));
    }
    {
        // The refresh itself throws after both children returned; the baseline throws the same way.
        Arena    offMemory, onMemory;
        Behavior offModel, onModel;
        offMemory.setup();
        onMemory.setup();
        offModel.throwRefresh = onModel.throwRefresh = true;
        const auto       off                         = invoke(Entry::Refresh, refreshContext(), offMemory, offModel, false);
        VsReadbackAccess access{ Arena::Read, &onMemory };
        SetVsReadbackAccess(&access);
        const auto on = invoke(Entry::Refresh, refreshContext(), onMemory, onModel, true);
        SetVsReadbackAccess(nullptr);
        UNSCOPED_INFO("the same exception propagates exactly once");
        CHECK((off.threw && on.threw && off.what == on.what && off.originals == 1 && on.originals == 1));
        UNSCOPED_INFO("unwound context and memory match the baseline");
        CHECK(sameOutcome(off, on));
        const auto report = drain();
        UNSCOPED_INFO("unwind abandons only the throwing call");
        CHECK((report.counts.abandon[kRefresh] == 1 && report.counts.returned[kRefresh] == 0 && report.counts.returned[kQuery] == 1 && report.counts.returned[kCopy] == 1));
        UNSCOPED_INFO("pending reaches zero");
        CHECK((report.pending == Totals{} && report.counts.pairError == 0 && conserved(report)));
    }
    {
        Arena    memory;
        Behavior model;
        memory.setup();
        model.throwCopy = true;
        VsReadbackAccess access{ Arena::Read, &memory };
        SetVsReadbackAccess(&access);
        const auto outcome = invoke(Entry::Readback, copyContext(), memory, model, true);
        SetVsReadbackAccess(nullptr);
        UNSCOPED_INFO("standalone copy exception propagates");
        CHECK((outcome.threw && outcome.what == "original failed"));
        const auto report = drain();
        UNSCOPED_INFO("abandoned copy is not a return");
        CHECK((report.counts.abandon[kCopy] == 1 && report.counts.returned[kCopy] == 0 && report.pending == Totals{}));
        UNSCOPED_INFO("the request made at entry stays counted");
        CHECK(report.counts.requestedMetaBytes == kM);
    }
}

void testOverlap()
{
    drain();
    Arena    memory;
    Behavior model;
    memory.setup();
    // Output overlaps the code and holds the size word. No guest bytes there may be read.
    const auto destination = kCode + 0x10;
    memory.forbidden.emplace_back(kCode, kCode + kS + 0x200);
    memory.forbidden.emplace_back(destination, destination + kSum);
    model.destination = destination;
    VsReadbackAccess access{ Arena::Read, &memory };
    SetVsReadbackAccess(&access);
    const auto outcome = invoke(Entry::Readback, copyContext(destination + 4, destination), memory, model, true);
    SetVsReadbackAccess(nullptr);
    UNSCOPED_INFO("overlap neither suppresses nor retries Original");
    CHECK((outcome.originals == 1 && !outcome.threw && outcome.unchanged));
    UNSCOPED_INFO("diagnostics read neither code nor output bytes");
    CHECK(memory.forbiddenReads == 0);
    const auto  report = drain();
    const auto* copy   = detailOf(report, VsReadbackKind::Copy);
    UNSCOPED_INFO("overlap is one quality event");
    CHECK((report.counts.overlap == 1 && copy && (copy->quality & kVsReadbackQualityOverlap)));
    UNSCOPED_INFO("output intersects the code and the size word");
    CHECK((copy && (copy->extents.overlap & kVsReadbackOverlapOutputCode) && (copy->extents.overlap & kVsReadbackOverlapOutputSize)));
    UNSCOPED_INFO("metadata stays disjoint");
    CHECK((copy && !(copy->extents.overlap & kVsReadbackOverlapOutputMeta)));
    UNSCOPED_INFO("an overlapping header is still sampled; no copy-equivalence is claimed");
    CHECK((report.counts.copyHeaderValid == 1 && report.counts.requestedMetaBytes == kM));
}

void testStorageParity()
{
    // Correlation table full: 64 live refresh tokens on the published probe.
    drain();
    PPCContext                   contexts[kVsReadbackActive];
    std::vector<VsReadbackToken> tokens;
    Arena                        fixture;
    fixture.setup();
    VsReadbackAccess access{ Arena::Read, &fixture };
    for (unsigned i = 0; i < kVsReadbackActive; ++i)
    {
        contexts[i] = makeContext(0x9000 + i, 0x30000, 0, kO, kQ, 0);
        tokens.push_back(BeginVsReadbackProbe(PublishedVsReadbackProbe(), VsReadbackFunction::Refresh, contexts[i], access));
    }
    {
        Arena    offMemory, onMemory;
        Behavior offModel, onModel;
        offMemory.setup();
        onMemory.setup();
        const auto       off = invoke(Entry::Refresh, refreshContext(), offMemory, offModel, false);
        VsReadbackAccess named{ Arena::Read, &onMemory };
        SetVsReadbackAccess(&named);
        const auto on = invoke(Entry::Refresh, refreshContext(), onMemory, onModel, true);
        SetVsReadbackAccess(nullptr);
        UNSCOPED_INFO("full correlation table leaves Original and parity intact");
        CHECK((sameOutcome(off, on) && on.originals == 1));
    }
    for (unsigned i = 0; i < kVsReadbackActive; ++i)
        EndVsReadbackProbe(PublishedVsReadbackProbe(), tokens[i], contexts[i], access);
    auto report = drain();
    UNSCOPED_INFO("one refresh was denied correlation");
    CHECK((report.counts.correlationDrop == 1 && report.untrackedRefresh == 0));
    UNSCOPED_INFO("balance after a full table");
    CHECK((report.counts.pairError == 0 && report.pending == Totals{} && conserved(report)));
    // Detail ring full.
    for (unsigned i = 0; i < kVsReadbackDetails + 2; ++i)
    {
        Arena    memory;
        Behavior model;
        memory.setup();
        VsReadbackAccess named{ Arena::Read, &memory };
        SetVsReadbackAccess(&named);
        const auto on = invoke(Entry::Readback, queryContext(), memory, model, true);
        SetVsReadbackAccess(nullptr);
        UNSCOPED_INFO("Original runs with a full detail ring");
        CHECK((on.originals == 1 && !on.threw));
    }
    report = drain();
    UNSCOPED_INFO("full ring keeps every total and reports the drops");
    CHECK((report.counts.enter[kQuery] == kVsReadbackDetails + 2 && report.counts.returned[kQuery] == kVsReadbackDetails + 2 && report.counts.eventDrop == 2 && report.detailCount == kVsReadbackDetails));
}

// ---------------------------------------------------------------------------

std::vector<std::pair<bool, std::string>> logged;
bool                                      failCountsLog = false, failDetailLog = false;

void captureLog(bool detail, std::string_view line)
{
    if ((!detail && failCountsLog) || (detail && failDetailLog))
        throw std::runtime_error("log failed");
    logged.emplace_back(detail, std::string(line));
}

bool contains(const std::string& text, std::string_view needle)
{
    return text.find(needle) != std::string::npos;
}

void testReporting()
{
    enable(true);
    Arena memory;
    memory.setup();
    {
        // Shutdown without any timing report keeps every record and shows a live pending call.
        VsReadbackProbe probe;
        PPCContext      rc, qc, live;
        auto            parent = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        auto            child  = beginChild(probe, memory, 0x7000, 0x2FF70, true, kVs, kVsReadbackQueryLr, qc);
        endCall(probe, memory, child, qc);
        auto pending = beginRefresh(probe, memory, 0x7001, 0x38000, kQ, live);
        (void)parent;
        logged.clear();
        VsReadbackReportOperations operations;
        operations.probe  = &probe;
        operations.log    = captureLog;
        const auto fields = ReportVsReadbackProbe(true, operations);
        UNSCOPED_INFO("shutdown-only report keeps late counts");
        CHECK((contains(fields, " rb_read_refresh_enter=2 ") && contains(fields, " rb_read_refresh_pending=2 ") && contains(fields, " rb_read_query_return=1 ")));
        UNSCOPED_INFO("pending calls make the final conservation explicit");
        CHECK((contains(fields, " rb_read_refresh_enter_total=2 rb_read_refresh_return_total=0 rb_read_refresh_abandon_total=0 ") && contains(fields, " rb_read_conserved=1 ")));
        const auto details = std::count_if(logged.begin(), logged.end(), [](const auto& line)
                                           {
                                               return line.first;
                                           });
        UNSCOPED_INFO("one query detail, two pending refresh lines, one shutdown line");
        CHECK((details == 3 && contains(logged.back().second, "shutdown") && !logged.back().first));
        UNSCOPED_INFO("pending refresh is listed, not invented as complete");
        CHECK(std::any_of(logged.begin(), logged.end(), [](const auto& line)
                          {
                              return line.first && contains(line.second, "terminal=pending");
                          }));
        UNSCOPED_INFO("generation and outer activation stay unknown");
        CHECK(std::any_of(logged.begin(), logged.end(), [](const auto& line)
                          {
                              return line.first && contains(line.second, "generation=unknown outer_activation=unknown");
                          }));
        endCall(probe, memory, pending, live);
        (void)probe.Take();
    }
    {
        // Events after the last timing report reach the final report once.
        VsReadbackProbe            probe;
        PPCContext                 rc;
        VsReadbackReportOperations operations;
        operations.probe = &probe;
        operations.log   = captureLog;
        auto early       = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        endCall(probe, memory, early, rc);
        logged.clear();
        const auto periodic = ReportVsReadbackProbe(false, operations);
        UNSCOPED_INFO("periodic report returns fields for the timing line and logs details only");
        CHECK((contains(periodic, " rb_read_refresh_enter=1 ") && !logged.empty() && logged.back().first));
        auto late = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        endCall(probe, memory, late, rc);
        logged.clear();
        const auto final = ReportVsReadbackProbe(true, operations);
        UNSCOPED_INFO("last event after the last timing report is retained, totals are cumulative");
        CHECK((contains(final, " rb_read_refresh_enter=1 ") && contains(final, " rb_read_refresh_enter_total=2 rb_read_refresh_return_total=2 ")));
        UNSCOPED_INFO("the late detail is logged once, the early one was not repeated");
        CHECK(std::count_if(logged.begin(), logged.end(), [](const auto& line)
                            {
                                return line.first;
                            }) == 1);
        const auto after = ReportVsReadbackProbe(true, operations);
        UNSCOPED_INFO("a second final report repeats no interval counts");
        CHECK((contains(after, " rb_read_refresh_enter=0 ") && contains(after, " rb_read_refresh_enter_total=2 ")));
    }
    {
        // A failed handoff retains the interval counts for the next drain.
        VsReadbackProbe            probe;
        PPCContext                 rc;
        VsReadbackReportOperations operations;
        operations.probe = &probe;
        operations.log   = captureLog;
        auto token       = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        endCall(probe, memory, token, rc);
        failCountsLog     = true;
        const auto failed = ReportVsReadbackProbe(true, operations);
        failCountsLog     = false;
        UNSCOPED_INFO("a throwing log returns no fields");
        CHECK(failed.empty());
        const auto retry = ReportVsReadbackProbe(true, operations);
        UNSCOPED_INFO("the failed batch and its failure count reach the next report");
        CHECK((contains(retry, " rb_read_refresh_enter=1 ") && contains(retry, " rb_read_reporting_failures=1 ")));
    }
    {
        // A detail line that cannot be logged is a counted reporting failure.
        VsReadbackProbe            probe;
        PPCContext                 rc;
        VsReadbackReportOperations operations;
        operations.probe = &probe;
        operations.log   = captureLog;
        auto token       = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        endCall(probe, memory, token, rc);
        failDetailLog       = true;
        const auto periodic = ReportVsReadbackProbe(false, operations);
        failDetailLog       = false;
        UNSCOPED_INFO("a lost detail line keeps the counts and is counted in the same report");
        CHECK((contains(periodic, " rb_read_refresh_enter=1 ") && contains(periodic, " rb_read_reporting_failures=1 ")));
        const auto next = ReportVsReadbackProbe(false, operations);
        UNSCOPED_INFO("the lost detail line is not counted twice");
        CHECK(contains(next, " rb_read_reporting_failures=0 "));
    }
    {
        // The shutdown line counts its own lost detail and pending-record lines.
        VsReadbackProbe            probe;
        PPCContext                 rc, live;
        VsReadbackReportOperations operations;
        operations.probe = &probe;
        operations.log   = captureLog;
        auto done        = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
        endCall(probe, memory, done, rc);
        auto pending = beginRefresh(probe, memory, 0x7001, 0x38000, kQ, live);
        logged.clear();
        failDetailLog    = true;
        const auto final = ReportVsReadbackProbe(true, operations);
        failDetailLog    = false;
        UNSCOPED_INFO("one lost detail and one lost pending record are in the final fields");
        CHECK(contains(final, " rb_read_reporting_failures=2 "));
        UNSCOPED_INFO("the shutdown line itself reports the lost records");
        CHECK((!logged.empty() && !logged.back().first && contains(logged.back().second, " rb_read_reporting_failures=2 ")));
        endCall(probe, memory, pending, live);
        (void)probe.Take();
    }
    {
        // The conservation flag reports a violated equation instead of assuming it.
        VsReadbackReport broken;
        broken.enterTotal[kCopy]  = 3;
        broken.returnTotal[kCopy] = 1;
        UNSCOPED_INFO("a violated equation clears rb_read_conserved");
        CHECK(contains(FormatVsReadbackCounts(broken), " rb_read_conserved=0 "));
        broken.abandonTotal[kCopy] = 1;
        broken.pending[kCopy]      = 1;
        UNSCOPED_INFO("a balanced equation sets rb_read_conserved");
        CHECK(contains(FormatVsReadbackCounts(broken), " rb_read_conserved=1 "));
    }
    enable(false);
}

struct Held
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false, released = false;
};

Held* held = nullptr;

void holdLog(bool detail, std::string_view)
{
    if (detail || !held)
        return;
    std::unique_lock lock(held->mutex);
    held->entered = true;
    held->changed.notify_all();
    held->changed.wait(lock, []
                       {
                           return held->released;
                       });
}

void testFinalLockDeadline()
{
    enable(true);
    Arena memory;
    memory.setup();
    VsReadbackProbe probe;
    PPCContext      rc;
    auto            token = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    endCall(probe, memory, token, rc);
    Held h;
    held = &h;
    VsReadbackReportOperations holding;
    holding.probe = &probe;
    holding.log   = holdLog;
    // The periodic reporter logs counts only at shutdown, so hold through a final report.
    std::thread stuck([&]
                      {
                          ReportVsReadbackProbe(true, holding);
                      });
    {
        std::unique_lock lock(h.mutex);
        UNSCOPED_INFO("first reporter holds the reporting lock");
        CHECK(h.changed.wait_for(lock, std::chrono::seconds(5), [&]
                                 {
                                     return h.entered;
                                 }));
    }
    VsReadbackReportOperations plain;
    plain.probe = &probe;
    plain.log   = captureLog;
    logged.clear();
    std::promise<std::string> result;
    auto                      ready = result.get_future();
    std::thread               second([&]
                                     {
                           result.set_value(ReportVsReadbackProbe(true, plain));
                                     });
    const bool                bounded = ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    UNSCOPED_INFO("a held reporting lock cannot block the final report beyond its bound");
    CHECK(bounded);
    {
        std::lock_guard lock(h.mutex);
        h.released = true;
    }
    h.changed.notify_all();
    stuck.join();
    second.join();
    const auto marker = ready.get();
    UNSCOPED_INFO("a skipped final report returns an incomplete marker");
    CHECK(contains(marker, " rb_read_final_incomplete=1 rb_read_final_skipped=1"));
    UNSCOPED_INFO("the skipped final report is logged");
    CHECK((!logged.empty() && contains(logged.back().second, "rb_read_final_incomplete=1")));
    const auto retry = ReportVsReadbackProbe(true, plain);
    UNSCOPED_INFO("the skipped count is reported on the next report");
    CHECK(contains(retry, " rb_read_final_skipped=1 rb_read_final_incomplete=1"));
    const auto clean = ReportVsReadbackProbe(true, plain);
    UNSCOPED_INFO("the count is cleared once reported");
    CHECK(contains(clean, " rb_read_final_skipped=0 rb_read_final_incomplete=0"));
    held = nullptr;
    enable(false);
}

struct RetainGate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false, held = false, thrown = false;
};

RetainGate* gate = nullptr;

// Fails the first final line once the test holds the recorder lock, so the
// reporter's retention attempt runs against a lock that never frees.
void gateLog(bool detail, std::string_view line)
{
    logged.emplace_back(detail, std::string(line));
    if (detail || !gate || gate->thrown)
        return;
    std::unique_lock lock(gate->mutex);
    gate->thrown  = true;
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, []
                       {
                           return gate->held;
                       });
    throw std::runtime_error("log failed");
}

void testRetainDeadline()
{
    enable(true);
    Arena memory;
    memory.setup();
    VsReadbackProbe probe;
    PPCContext      rc;
    auto            token = beginRefresh(probe, memory, 0x7000, 0x30000, kQ, rc);
    endCall(probe, memory, token, rc);
    RetainGate g;
    gate = &g;
    logged.clear();
    VsReadbackReportOperations operations;
    operations.probe = &probe;
    operations.log   = gateLog;
    std::promise<std::string> result;
    auto                      ready = result.get_future();
    std::thread               reporter([&]
                                       {
                             result.set_value(ReportVsReadbackProbe(true, operations));
                                       });
    {
        std::unique_lock lock(g.mutex);
        UNSCOPED_INFO("the final line is being logged");
        CHECK(g.changed.wait_for(lock, std::chrono::seconds(5), [&]
                                 {
                                     return g.entered;
                                 }));
    }
    std::unique_lock recorder(VsReadbackProbeTestAccess::RecorderLock(probe));
    {
        std::lock_guard lock(g.mutex);
        g.held = true;
    }
    g.changed.notify_all();
    const bool bounded = ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    UNSCOPED_INFO("a held recorder lock cannot block the final retention beyond its bound");
    CHECK(bounded);
    recorder.unlock();
    reporter.join();
    const auto marker = ready.get();
    gate              = nullptr;
    UNSCOPED_INFO("a final batch that could not be retained marks the line incomplete");
    CHECK(contains(marker, " rb_read_final_incomplete=1 rb_read_final_skipped=1"));
    UNSCOPED_INFO("the incomplete line is logged");
    CHECK((!logged.empty() && !logged.back().first && contains(logged.back().second, "rb_read_final_incomplete=1")));
    logged.clear();
    VsReadbackReportOperations plain;
    plain.probe     = &probe;
    plain.log       = captureLog;
    const auto next = ReportVsReadbackProbe(true, plain);
    UNSCOPED_INFO("the skipped final batch is counted at the next report");
    CHECK(contains(next, " rb_read_final_skipped=1 rb_read_final_incomplete=1"));
    enable(false);
}

void testTokenOwnership()
{
    static_assert(!std::is_copy_constructible_v<VsReadbackToken> && !std::is_copy_assignable_v<VsReadbackToken>, "a live call has one token");
    static_assert(std::is_nothrow_move_constructible_v<VsReadbackToken> && !std::is_move_assignable_v<VsReadbackToken>, "tokens move-construct and never assign over a live call");
    Arena memory;
    memory.setup();
    VsReadbackProbe probe;
    PPCContext      other, first, second;
    // Another query stays live while the moved-from token is ended again.
    auto live  = beginChild(probe, memory, 0x7001, 0x38000, true, kVs, kVsReadbackQueryLr, other);
    auto moved = beginChild(probe, memory, 0x7000, 0x30000, true, kVs, kVsReadbackQueryLr, first);
    auto owner = std::move(moved);
    UNSCOPED_INFO("a moved-from token is finished");
    CHECK((moved.done && !moved.counted));
    endCall(probe, memory, owner, first);
    endCall(probe, memory, moved, first);
    auto report = probe.Take();
    UNSCOPED_INFO("ending a moved-from token takes no pending slot from another call");
    CHECK((report.enterTotal[kQuery] == 2 && report.returnTotal[kQuery] == 1 && report.pending[kQuery] == 1));
    UNSCOPED_INFO("the stale end is a pair error with no return");
    CHECK((report.counts.pairError == 1 && report.counts.returned[kQuery] == 1));
    auto again   = beginChild(probe, memory, 0x7002, 0x3A000, true, kVs, kVsReadbackQueryLr, second);
    auto adopted = std::move(again);
    UNSCOPED_INFO("move construction transfers the live call");
    CHECK((again.done && !adopted.done && adopted.counted));
    endCall(probe, memory, adopted, second);
    endCall(probe, memory, adopted, second);
    endCall(probe, memory, again, second);
    endCall(probe, memory, live, other);
    report = probe.Take();
    UNSCOPED_INFO("a repeated end cannot underflow or double count");
    CHECK((report.pending == Totals{} && report.returnTotal[kQuery] == 3 && report.enterTotal[kQuery] == 3 && conserved(report)));
    UNSCOPED_INFO("each stale end is one pair error");
    CHECK(report.counts.pairError == 2);
}

void testHookStructure()
{
    // The wrappers fall back to Original when the flag is off even if a reader is installed.
    CountingReader         reader;
    const VsReadbackAccess access{ CountingReader::Read, &reader };
    SetVsReadbackAccess(&access);
    Arena    memory;
    Behavior model;
    memory.setup();
    const auto outcome = invoke(Entry::Refresh, refreshContext(), memory, model, false);
    SetVsReadbackAccess(nullptr);
    UNSCOPED_INFO("cached flag is checked before any scope begins");
    CHECK((outcome.originals == 1 && reader.calls == 0));
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 22>;

const CaseTable kCases{
    CaseTable::value_type{ "testCvarDefault", testCvarDefault },
    CaseTable::value_type{ "testDisabledPath", testDisabledPath },
    CaseTable::value_type{ "testHookStructure", testHookStructure },
    CaseTable::value_type{ "testWrapperParity", testWrapperParity },
    CaseTable::value_type{ "testRegisterParity", testRegisterParity },
    CaseTable::value_type{ "testRefreshRoundTrip", testRefreshRoundTrip },
    CaseTable::value_type{ "testQueryClassification", testQueryClassification },
    CaseTable::value_type{ "testCopySizePointer", testCopySizePointer },
    CaseTable::value_type{ "testEndianExtents", testEndianExtents },
    CaseTable::value_type{ "testInvalidAndWrap", testInvalidAndWrap },
    CaseTable::value_type{ "testQdirtyAndCallers", testQdirtyAndCallers },
    CaseTable::value_type{ "testCorrelation", testCorrelation },
    CaseTable::value_type{ "testDrainSurvival", testDrainSurvival },
    CaseTable::value_type{ "testCapacity", testCapacity },
    CaseTable::value_type{ "testPairErrors", testPairErrors },
    CaseTable::value_type{ "testUnwind", testUnwind },
    CaseTable::value_type{ "testOverlap", testOverlap },
    CaseTable::value_type{ "testStorageParity", testStorageParity },
    CaseTable::value_type{ "testReporting", testReporting },
    CaseTable::value_type{ "testFinalLockDeadline", testFinalLockDeadline },
    CaseTable::value_type{ "testRetainDeadline", testRetainDeadline },
    CaseTable::value_type{ "testTokenOwnership", testTokenOwnership }
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

TEST_CASE("vs_readback_probe", "[ctest][vs-readback-probe][timeout-60]")
{
    runCases({});
}

// Runs one named case:
//   vs-readback-probe.case <testName>
TEST_CASE("vs_readback_probe_selected", "[.][manual][vs-readback-probe]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(vsReadbackProbeCase, "vs-readback-probe.case", "vs_readback_probe_selected")
