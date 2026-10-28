#include "unit_sale_value.h"

#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <rex/ppc.h>

#if defined(_WIN32)
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#endif

void BeginUnitSalePreview(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);
void ApplyUnitSalePreview(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);
void BeginUnitSaleCredit(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);
void ApplyUnitSaleCredit(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);

namespace
{
namespace sale            = rerevved::unit_sale;
constexpr uint32_t kFrame = 0x40008000;
constexpr uint32_t kPlane = sale::kUnitBase + 2 * 256 * 84;

void putWord(uint8_t* out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (24 - 8 * i));
    }
}

uint32_t getWord(const uint8_t* in)
{
    return uint32_t{ in[0] } << 24 | uint32_t{ in[1] } << 16 | uint32_t{ in[2] } << 8 | in[3];
}

struct Fixture final : sale::Memory
{
    std::array<uint8_t, 84 * 256> units{};
    std::array<uint8_t, 148 * 38> definitions{};
    std::array<uint8_t, 8>        keys{};
    std::array<uint8_t, 4>        saved{};
    PPCContext                    context{};
    bool                          failRead   = false;
    bool                          throwRead  = false;
    int                           failOnRead = 0;
    int                           reads      = 0;
    std::function<void()>         duringRead;

    Fixture()
    {
        for (int i = 0; i < 256; ++i)
        {
            units[84 * i]        = 255;
            units[84 * i + 0x26] = 255;
            units[84 * i + 0x27] = 255;
        }
        putWord(keys.data(), 2);
        putWord(keys.data() + 4, 5);
        putWord(saved.data(), 5);
        Unit(5, 21, 1, -1);
        definitions[21 * 148 + 0x44] = 6;
        definitions[28 * 148 + 0x44] = 5;
        putWord(definitions.data() + 21 * 148 + 0x50, 0x10);
        context.r0.u64  = 0x1122334455667788;
        context.r1.u64  = kFrame;
        context.r2.u64  = 0x8877665544332211;
        context.r4.u64  = 0xDEADBEEFDEADBEEF;
        context.r15.u64 = sale::kUnitBase;
        context.r17.u64 = 2;
        context.r19.u64 = 0x80000000;
        context.r21.u64 = sale::kDefinitionBase;
        context.r26.u64 = sale::kGlobalBase;
        context.r31.u64 = 5;
        context.lr      = 0x12345678;
        context.xer.ca  = 1;
        context.cr6.eq  = true;
        sale::SetMemoryForTesting(this);
    }

    ~Fixture() override
    {
        sale::SetMemoryForTesting(nullptr);
    }

    uint8_t* Unit(int slot, int type, int marker, int link, int owner = 2, uint32_t flags = 0)
    {
        auto* unit = units.data() + slot * 84;
        unit[0]    = static_cast<uint8_t>(owner);
        unit[1]    = static_cast<uint8_t>(type);
        unit[4]    = static_cast<uint8_t>(marker);
        putWord(unit + 0xC, flags);
        unit[0x26] = static_cast<uint8_t>(link >> 8);
        unit[0x27] = static_cast<uint8_t>(link);
        return unit;
    }

    bool Read(uint32_t address, void* out, uint32_t size) override
    {
        ++reads;
        if (duringRead)
        {
            auto callback = std::move(duringRead);
            duringRead    = {};
            callback();
        }
        if (throwRead)
        {
            throw std::runtime_error("unit sale fixture read");
        }
        if (failRead || reads == failOnRead)
        {
            return false;
        }
        const auto copy = [&](uint32_t start, const auto& bytes)
        {
            if (address < start || uint64_t{ address - start } + size > bytes.size())
            {
                return false;
            }
            std::memcpy(out, bytes.data() + address - start, size);
            return true;
        };
        return copy(kPlane, units) || copy(sale::kDefinitionBase, definitions) || copy(0x8312B8E8, keys) || copy(kFrame + 0x90, saved);
    }

    void PreparePreview()
    {
        auto& c   = context;
        c.r3.s64  = static_cast<int8_t>(units[5 * 84 + 1]);
        c.r7.u64  = 84 * ((2 << 8) + 5);
        c.r11.u64 = 148 * c.r3.u64;
        c.r10.u64 = getWord(definitions.data() + c.r11.u32 + 0x50);
    }

    void CallBegin(bool credit)
    {
        auto& c = context;
        if (credit)
        {
            BeginUnitSaleCredit(c.r1, c.r15, c.r17, c.r19, c.r21, c.r26, c.r31);
        }
        else
        {
            BeginUnitSalePreview(c.r1, c.r3, c.r7, c.r10, c.r11, c.r15, c.r17, c.r19, c.r21, c.r26, c.r31);
        }
    }

    void Begin(bool credit)
    {
        if (!credit)
        {
            PreparePreview();
        }
        CallBegin(credit);
    }

    void Native(bool credit, int h = 1)
    {
        const auto factor = [&](int type)
        {
            return type == 7 ? 0 : static_cast<int8_t>(definitions[type * 148 + 0x44]);
        };
        const int type = static_cast<int8_t>(units[5 * 84 + 1]);
        int       sum  = factor(type);
        if (getWord(definitions.data() + type * 148 + 0x50) & 0x10)
        {
            for (int i = 0; i < 256; ++i)
            {
                const auto* unit = units.data() + 84 * i;
                const int   link = static_cast<int16_t>(uint16_t{ unit[0x26] } << 8 | unit[0x27]);
                if (unit[0] != 255 && !(getWord(unit + 0xC) & 0x80000000) && link == 5 && unit[1] != 7)
                {
                    sum += factor(static_cast<int8_t>(unit[1]));
                }
            }
        }
        const int multiplier = units[5 * 84 + 4] ? 3 : 1;
        // Execute the native multiply, masked shift and full add contract.
        const int64_t  product = sum * multiplier * (credit ? h : 1);
        const uint64_t amount  = static_cast<uint64_t>(product) + static_cast<uint32_t>(static_cast<uint32_t>(product) << 2);
        if (credit)
        {
            context.r5.u64  = amount;
            context.r8.s64  = h;
            context.r30.s64 = sum;
            context.r29.u64 = sale::kGlobalBase;
            context.r31.u64 = sale::kGlobalBase;
        }
        else
        {
            context.r3.s64 = type;
            context.r5.s64 = multiplier;
            context.r6.u64 = amount;
            context.r9.s64 = sum;
        }
    }

    void Apply(bool credit, uint64_t expected)
    {
        const auto beforeUnits       = units;
        const auto beforeDefinitions = definitions;
        const auto beforeKeys        = keys;
        const auto beforeSaved       = saved;
        PPCContext expectedContext{};
        std::memcpy(&expectedContext, &context, sizeof(context));
        (credit ? expectedContext.r5 : expectedContext.r6).u64 = expected;
        auto& c                                                = context;
        if (credit)
        {
            ApplyUnitSaleCredit(c.r1, c.r5, c.r8, c.r15, c.r17, c.r19, c.r21, c.r29, c.r30, c.r31);
        }
        else
        {
            ApplyUnitSalePreview(c.r1, c.r3, c.r5, c.r6, c.r9, c.r15, c.r17, c.r19, c.r21, c.r26, c.r31);
        }
        CHECK(std::memcmp(&context, &expectedContext, sizeof(context)) == 0);
        CHECK(units == beforeUnits);
        CHECK(definitions == beforeDefinitions);
        CHECK(keys == beforeKeys);
        CHECK(saved == beforeSaved);
    }

    uint64_t Amount(bool credit) const
    {
        return credit ? context.r5.u64 : context.r6.u64;
    }
};

#if defined(_WIN32)
constexpr std::array<uint32_t, 5> kMappedPages{ 0x40000000, 0x82F70000, 0x830F0000, 0x83100000, 0x83120000 };

struct MappedSaleMemory
{
    rex::Runtime                               runtime{ std::filesystem::path{} };
    rex::runtime::ThreadState*                 previous = rex::runtime::ThreadState::Get();
    std::unique_ptr<rex::runtime::ThreadState> thread;

    MappedSaleMemory()
    {
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const auto page : kMappedPages)
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        thread = std::make_unique<rex::runtime::ThreadState>(1, kFrame, 0, runtime.memory());
        rex::runtime::ThreadState::Bind(thread.get());
    }

    ~MappedSaleMemory()
    {
        rex::runtime::ThreadState::Bind(previous);
    }

    uint8_t* Base()
    {
        return runtime.memory()->virtual_membase();
    }
};
#endif

} // namespace

TEST_CASE("unit sale values each extant record independently", "[ctest][unit-sale]")
{
    struct Row
    {
        int      carrier;
        int      ordinary;
        int      armies;
        uint64_t expected;
        uint64_t doubled;
    };

    constexpr Row rows[]{ { 0, 0, 0, 30, 60 }, { 1, 0, 0, 90, 180 }, { 0, 1, 0, 55, 110 }, { 1, 1, 0, 115, 230 }, { 1, 10, 0, 340, 680 }, { 0, 0, 1, 105, 210 }, { 1, 0, 1, 165, 330 }, { 1, 1, 1, 190, 380 } };
    for (const auto& row : rows)
    {
        for (const bool credit : { false, true })
        {
            for (const int h : { 1, 2 })
            {
                CAPTURE(row.carrier, row.ordinary, row.armies, credit, h);
                Fixture f;
                f.units[5 * 84 + 4] = static_cast<uint8_t>(row.carrier);
                int slot            = 6;
                for (int i = 0; i < row.ordinary; ++i)
                {
                    f.Unit(slot++, 28, 0, 5);
                }
                for (int i = 0; i < row.armies; ++i)
                {
                    f.Unit(slot++, 28, 1, 5);
                }
                f.Begin(credit);
                f.Native(credit, h);
                f.Apply(credit, credit && h == 2 ? row.doubled : row.expected);
            }
        }
    }
}

TEST_CASE("unit sale preserves native membership and marker predicates", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (const int marker : { 0, 1, 128, 255 })
        {
            CAPTURE(credit, marker);
            Fixture f;
            f.units[5 * 84 + 4] = static_cast<uint8_t>(marker);
            f.Unit(6, 28, marker, 5, 4); // Native admits another owner in this plane.
            f.Unit(7, 7, 255, 5);
            f.definitions[7 * 148 + 0x44] = 127;
            f.Unit(8, 255, 255, 5, 255);
            f.Unit(9, 38, 255, 5, 2, 0x80000000);
            f.Unit(10, 255, 255, -1);
            f.units[6 * 84 + 5] = 255;
            putWord(f.units.data() + 10 * 84 + 0x50, 5);
            f.Begin(credit);
            f.Native(credit);
            f.Apply(credit, marker ? 165 : 55);
        }
    }
}

TEST_CASE("unit sale negative factors retain full native register representation", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (const int h : { 1, 2 })
        {
            Fixture f;
            f.definitions[21 * 148 + 0x44] = static_cast<uint8_t>(-6);
            f.Unit(6, 28, 0, 5);
            f.Begin(credit);
            f.Native(credit, h);
            f.Apply(credit, credit && h == 2 ? 0xFFFFFF7Eull : 0xFFFFFFBFull);
        }
    }
    Fixture militia;
    militia.Unit(5, 7, 255, -1);
    militia.Begin(false);
    militia.Native(false);
    militia.Apply(false, 0);
}

TEST_CASE("unit sale rejects changed roster even when the native sum agrees", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (int mutation = 0; mutation < 9; ++mutation)
        {
            CAPTURE(credit, mutation);
            Fixture f;
            f.Unit(6, 28, 0, 5);
            f.Begin(credit);
            f.Native(credit);
            switch (mutation)
            {
                case 0:
                    f.Unit(6, 28, 0, -1);
                    f.Unit(7, 28, 0, 5);
                    break;
                case 1:
                    f.units[6 * 84 + 4] = 1;
                    break;
                case 2:
                    f.units[6 * 84 + 1]            = 29;
                    f.definitions[29 * 148 + 0x44] = 5;
                    break;
                case 3:
                    f.units[6 * 84] = 3;
                    break;
                case 4:
                    putWord(f.units.data() + 6 * 84 + 0xC, 0x80000000);
                    break;
                case 5:
                    f.definitions[28 * 148 + 0x44] = 6;
                    break;
                case 6:
                    putWord(f.definitions.data() + 28 * 148 + 0x50, 0x10);
                    break;
                case 7:
                    f.units[5 * 84 + 4] = 0;
                    break;
                case 8:
                    putWord(f.keys.data() + 4, 6);
                    putWord(f.saved.data(), 6);
                    break;
            }
            f.Apply(credit, f.Amount(credit));
        }
    }
}

TEST_CASE("unit sale begin validates native identity before collecting", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (int mutation = 0; mutation < (credit ? 9 : 13); ++mutation)
        {
            CAPTURE(credit, mutation);
            Fixture f;
            f.Unit(6, 28, 0, 5);
            f.PreparePreview();
            switch (mutation)
            {
                case 0:
                    f.context.r31.u64 = 6;
                    break;
                case 1:
                    f.context.r17.u64 = 8;
                    break;
                case 2:
                    putWord(f.keys.data(), 8);
                    break;
                case 3:
                    putWord(f.keys.data() + 4, 256);
                    break;
                case 4:
                    putWord(f.saved.data(), 6);
                    break;
                case 5:
                    f.context.r15.u64 = 0;
                    break;
                case 6:
                    f.context.r21.u64 = 0;
                    break;
                case 7:
                    f.context.r26.u64 = 0;
                    break;
                case 8:
                    f.context.r1.u64 = 0xFFFFFFF0;
                    break;
                case 9:
                    f.context.r3.s64 = 28;
                    break;
                case 10:
                    f.context.r7.u64 += 84;
                    break;
                case 11:
                    f.context.r11.u64 += 148;
                    break;
                case 12:
                    f.context.r10.u64 = 0;
                    break;
            }
            f.CallBegin(credit);
            f.context.r1.u64  = kFrame;
            f.context.r15.u64 = sale::kUnitBase;
            f.context.r17.u64 = 2;
            f.context.r21.u64 = sale::kDefinitionBase;
            f.context.r26.u64 = sale::kGlobalBase;
            f.context.r31.u64 = 5;
            putWord(f.keys.data(), 2);
            putWord(f.keys.data() + 4, 5);
            putWord(f.saved.data(), 5);
            f.Native(credit);
            f.Apply(credit, 165);
        }
    }
}

TEST_CASE("unit sale failed reads at each sampled span cannot leave a receipt", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (const bool failApply : { false, true })
        {
            for (const int read : { 1, 2, 3, 4 })
            {
                CAPTURE(credit, failApply, read);
                Fixture f;
                f.Unit(6, 28, 0, 5);
                f.failOnRead = failApply ? 0 : read;
                f.Begin(credit);
                f.Native(credit);
                f.reads      = 0;
                f.failOnRead = failApply ? read : 0;
                f.Apply(credit, 165);
                f.failOnRead = 0;
                f.Apply(credit, 165);
            }
        }
    }
}

TEST_CASE("unit sale declines identity arithmetic and unreadable mismatches", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (int mutation = 0; mutation < 16; ++mutation)
        {
            CAPTURE(credit, mutation);
            Fixture f;
            f.Unit(6, 28, 0, 5);
            f.Begin(credit);
            f.Native(credit);
            switch (mutation)
            {
                case 0:
                    f.context.r15.u64 += 84;
                    break;
                case 1:
                    f.context.r21.u64 += 148;
                    break;
                case 2:
                    f.context.r19.u64 = 0;
                    break;
                case 3:
                    f.context.r17.u64 = 3;
                    break;
                case 4:
                    putWord(f.keys.data(), 3);
                    break;
                case 5:
                    putWord(f.saved.data(), 6);
                    break;
                case 6:
                    f.context.r1.u64 += 16;
                    break;
                case 7:
                    (credit ? f.context.r30 : f.context.r9).s64 += 1;
                    break;
                case 8:
                    (credit ? f.context.r5 : f.context.r6).u64 |= 0x1234000000000000ull;
                    break;
                case 9:
                    f.failRead = true;
                    break;
                case 10:
                    f.throwRead = true;
                    break;
                case 11:
                    if (credit)
                    {
                        f.context.r8.u64 = 3;
                    }
                    else
                    {
                        f.context.r5.u64 = 1;
                    }
                    break;
                case 12:
                    if (credit)
                    {
                        f.context.r29.u64 = 0;
                    }
                    else
                    {
                        f.context.r3.u64 = 28;
                    }
                    break;
                case 13:
                    if (credit)
                    {
                        f.context.r31.u64 = 5;
                    }
                    else
                    {
                        f.context.r31.u64 = 6;
                    }
                    break;
                case 14:
                    if (credit)
                    {
                        f.context.r8.u64 |= 0x100000000ull;
                    }
                    else
                    {
                        f.context.r9.u64 |= 0x100000000ull;
                    }
                    break;
                case 15:
                    f.context.r26.u64 = 0;
                    if (credit)
                    {
                        f.context.r31.u64 = 0;
                    }
                    break;
            }
            f.Apply(credit, f.Amount(credit));
        }
    }
}

TEST_CASE("unit sale falls back for nested self and unsupported cargo", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (const int type : { 21, 38, 255 })
        {
            CAPTURE(credit, type);
            Fixture f;
            f.Unit(6, type, 0, 5);
            f.Begin(credit);
            if (type == 21)
            {
                f.Native(credit);
                f.Apply(credit, 180);
            }
            else
            {
                (credit ? f.context.r5 : f.context.r6).u64 = 0xFEDCBA9876543210ull;
                f.Apply(credit, 0xFEDCBA9876543210ull);
            }
        }
        Fixture self;
        self.Unit(5, 21, 1, 5);
        self.Unit(6, 28, 0, 5);
        self.Begin(credit);
        self.Native(credit);
        self.Apply(credit, 255);
    }
}

TEST_CASE("unit sale pending collection cannot cross nested failed or unmatched seams", "[ctest][unit-sale]")
{
    for (const bool credit : { false, true })
    {
        for (int interruption = 0; interruption < 6; ++interruption)
        {
            CAPTURE(credit, interruption);
            Fixture f;
            f.Unit(6, 28, 0, 5);
            if (interruption == 0)
            {
                f.failRead = true;
            }
            if (interruption == 1)
            {
                f.throwRead = true;
            }
            if (interruption == 2)
            {
                f.duringRead = [&]
                {
                    f.Begin(credit);
                };
            }
            if (interruption != 5)
            {
                f.Begin(credit);
            }
            f.failRead  = false;
            f.throwRead = false;
            if (interruption == 3)
            {
                f.context.r1.u64 -= 0x100;
                f.Begin(credit);
                f.context.r1.u64 += 0x100;
            }
            f.Native(credit);
            if (interruption == 4)
            {
                f.duringRead = [&]
                {
                    f.context.r31.u64 = 5;
                    f.Begin(credit);
                    f.context.r31.u64 = credit ? sale::kGlobalBase : 5;
                };
            }
            f.Apply(credit, 165);
            f.Apply(credit, 165);
        }
    }
}

TEST_CASE("unit sale preview cannot donate state to declined or mismatched credit", "[ctest][unit-sale]")
{
    Fixture declined;
    declined.Unit(6, 28, 0, 5);
    declined.Begin(false);
    declined.Native(false);
    declined.Apply(false, 115);
    declined.Native(true, 2);
    declined.Apply(true, 330);

    Fixture phase;
    phase.Unit(6, 28, 0, 5);
    phase.Begin(false);
    phase.Native(true);
    phase.Apply(true, 165);
    phase.Apply(true, 165);
}

TEST_CASE("unit sale independent credit ingress needs its actual retained slot", "[ctest][unit-sale]")
{
    Fixture f;
    f.Unit(6, 28, 0, 5);
    f.context.r31.u64 = 6;
    f.Begin(true);
    f.Native(true, 2);
    f.Apply(true, 330);
    f.context.r31.u64 = 5;
    f.Begin(true);
    f.Native(true, 2);
    f.Apply(true, 230);
}

TEST_CASE("unit sale production reader admits mapped guest state", "[ctest][unit-sale]")
{
#if defined(_WIN32)
    MappedSaleMemory mapped;
    Fixture          fixture;
    fixture.Unit(6, 28, 0, 5);
    sale::SetMemoryForTesting(nullptr);
    auto* base = mapped.Base();
    std::memcpy(base + kPlane, fixture.units.data(), fixture.units.size());
    std::memcpy(base + sale::kDefinitionBase, fixture.definitions.data(), fixture.definitions.size());
    std::memcpy(base + 0x8312B8E8, fixture.keys.data(), fixture.keys.size());
    std::memcpy(base + kFrame + 0x90, fixture.saved.data(), fixture.saved.size());

    std::vector<uint8_t> guestBefore(kMappedPages.size() * 0x10000);
    for (size_t i = 0; i < kMappedPages.size(); ++i)
    {
        std::memcpy(guestBefore.data() + i * 0x10000, base + kMappedPages[i], 0x10000);
    }
    const auto checkGuest = [&]
    {
        for (size_t i = 0; i < kMappedPages.size(); ++i)
        {
            CHECK(std::memcmp(guestBefore.data() + i * 0x10000, base + kMappedPages[i], 0x10000) == 0);
        }
        CHECK(fixture.reads == 0);
    };
    auto&      context = *mapped.thread->context();
    const auto begin   = [&](bool credit)
    {
        fixture.context.r31.u64 = 5;
        if (!credit)
        {
            fixture.PreparePreview();
        }
        std::memcpy(&context, &fixture.context, sizeof(context));
        PPCContext expected{};
        std::memcpy(&expected, &context, sizeof(context));
        if (credit)
        {
            BeginUnitSaleCredit(context.r1, context.r15, context.r17, context.r19, context.r21, context.r26, context.r31);
        }
        else
        {
            BeginUnitSalePreview(context.r1, context.r3, context.r7, context.r10, context.r11, context.r15, context.r17, context.r19, context.r21, context.r26, context.r31);
        }
        CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
        checkGuest();
    };
    const auto apply = [&](bool credit, int h, uint64_t corrected)
    {
        fixture.Native(credit, h);
        REQUIRE(fixture.Amount(credit) == (credit && h == 2 ? 330 : 165));
        std::memcpy(&context, &fixture.context, sizeof(context));
        PPCContext expected{};
        std::memcpy(&expected, &context, sizeof(context));
        (credit ? expected.r5 : expected.r6).u64 = corrected;
        if (credit)
        {
            ApplyUnitSaleCredit(context.r1, context.r5, context.r8, context.r15, context.r17, context.r19, context.r21, context.r29, context.r30, context.r31);
        }
        else
        {
            ApplyUnitSalePreview(context.r1, context.r3, context.r5, context.r6, context.r9, context.r15, context.r17, context.r19, context.r21, context.r26, context.r31);
        }
        CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    };

    begin(false);
    apply(false, 1, 115);
    checkGuest();
    begin(true);
    apply(true, 1, 115);
    checkGuest();
    begin(true);
    apply(true, 2, 230);
    checkGuest();

    begin(true);
    auto* heap = mapped.runtime.memory()->LookupHeap(sale::kDefinitionBase);
    REQUIRE(heap->Protect(0x82F70000, 0x1000, rex::memory::kMemoryProtectWrite));
    apply(true, 1, 165);
    REQUIRE(heap->Protect(0x82F70000, 0x1000, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    checkGuest();
#else
    SKIP("Windows production guest-memory sampling");
#endif
}
