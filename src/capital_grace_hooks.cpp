#include "capital_grace_hooks.h"

#include "capital_grace.h"
#include "capital_grace_special.h"
#include "raging_barbarians_settings.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <stdexcept>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, raging_barbarians_trace);

namespace
{

namespace grace = rerevved::capital_grace;

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/barbarian-capital-grace-feasibility.json
// safeGateContract, scope.safeGatePackets and currentEvidence.safeGateSupport.
constexpr uint32_t kYear          = 0x8312B8E0;
constexpr uint32_t kBarbarian     = 0x82F700B0;
constexpr uint32_t kCurrentPlayer = 0x8312B8E8;
constexpr uint32_t kSelected      = 0x8312B8EC;
constexpr uint32_t kCache         = 0x830E33A8;
constexpr uint32_t kQueue         = 0x8314F1F8;
constexpr uint32_t kRing          = 0x83155C28;

struct NativeWord
{
    uint32_t address;
    uint32_t value;
};

// PowerPC encodings of the independently matched native instruction windows.
// Include every hook, each refusal destination and the result-1 dispatcher tail.
constexpr NativeWord kProfile[] = {
    { 0x82CDDF34, 0x4BFF6DBD }, // bl 0x82CD4CF0
    { 0x82CE16AC, 0x3D408315 }, // lis r10,-31979
    { 0x82CE16B0, 0x38600001 }, // li r3,1
    { 0x82CE16B4, 0x816A6344 }, // lwz r11,25412(r10)
    { 0x82CE16B8, 0x396BFFFF }, // addi r11,r11,-1
    { 0x82CE16BC, 0x916A6344 }, // stw r11,25412(r10)
    { 0x82CE16C0, 0x38210170 }, // addi r1,r1,368
    { 0x82CE16E8, 0x81090000 }, // lwz r8,0(r9)
    { 0x82CE16F4, 0x817FFFFC }, // lwz r11,-4(r31)
    { 0x82CE1730, 0x48000008 }, // b 0x82CE1738
    { 0x82CE1748, 0x556A06FE }, // clrlwi r10,r11,27
    { 0x82CE1784, 0x817DEFE0 }, // lwz r11,-4128(r29)
    { 0x82CE17EC, 0x38210090 }, // addi r1,r1,144
    { 0x82CE1828, 0x38210090 }, // addi r1,r1,144
    { 0x82D2C9BC, 0x4BFB4F95 }, // bl 0x82CE1950
    { 0x82D2C9C0, 0x4BFA8EF9 }, // bl 0x82CD58B8
    { 0x82D2CE00, 0x4BFB4B51 }, // bl 0x82CE1950
    { 0x82D2CE04, 0x3FA08313 }, // lis r29,-31981
    { 0x82D2C170, 0x4BFB57E1 }, // bl 0x82CE1950
    { 0x82D2C174, 0x4BFA9745 }, // bl 0x82CD58B8
    { 0x82D2BD84, 0x4BFB5BCD }, // bl 0x82CE1950
    { 0x82D2BD88, 0x3D608315 }, // lis r11,-31979
    { 0x82D2BBA0, 0x61280001 }, // ori r8,r9,1
    { 0x82D2BBA4, 0x7D1E292E }, // stwx r8,r30,r5
    { 0x82D2BDE0, 0x8177B8E8 }, // lwz r11,-18200(r23)
    { 0x82D2BDF8, 0x5507003C }, // rlwinm r7,r8,0,0,30
    { 0x82D2BDFC, 0x7CEB512E }, // stwx r7,r11,r10
    { 0x82D2BE00, 0x4BFFE90C }, // b 0x82D2A70C
    { 0x82D2D2E0, 0x38600001 }, // li r3,1
    { 0x82D1B43C, 0x7F139000 }, // cmpw cr6,r19,r18
    { 0x82D1D1B4, 0x38210140 }, // addi r1,r1,320
};

std::atomic<bool>     unavailableWarning{ false };
std::atomic<uint64_t> malformed{ 0 };
std::atomic<uint64_t> refusals{ 0 };

struct ThreadTokens
{
    rex::runtime::ThreadState* thread = nullptr;
    grace::Tokens              tokens;
};

thread_local ThreadTokens threadTokens;

const grace::QueueObserver* queueObserver() noexcept
{
    return grace::QueueObserverSlot().load(std::memory_order_acquire);
}

grace::Tokens& tokens() noexcept
{
    auto* current = rex::runtime::ThreadState::Get();
    if (threadTokens.thread != current)
    {
        threadTokens.tokens.Reset();
        threadTokens.thread = current;
    }
    return threadTokens.tokens;
}

uint8_t* checkedBytes(uint64_t address, uint32_t size, bool write = false)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || !size || address < 0x10000 || address + size > 0xFFFFF000ull)
    {
        throw std::runtime_error("capital grace: unavailable guest memory");
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + size - 1;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap)
    {
        throw std::runtime_error("capital grace: invalid guest range");
    }
    const auto access   = static_cast<uint32_t>(heap->QueryRangeAccess(first, last));
    const auto required = static_cast<uint32_t>(write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly);
    if ((access & required) != required)
    {
        throw std::runtime_error("capital grace: inaccessible guest range");
    }
    return memory->TranslateVirtual<uint8_t*>(first);
}

uint16_t half(const uint8_t* bytes) noexcept
{
    return (uint16_t{ bytes[0] } << 8) | bytes[1];
}

int16_t signedHalf(const uint8_t* bytes) noexcept
{
    return std::bit_cast<int16_t>(half(bytes));
}

uint32_t word(const uint8_t* bytes) noexcept
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
}

uint32_t readWord(uint64_t address)
{
    return word(checkedBytes(address, 4));
}

int32_t signedWord(uint64_t address)
{
    return std::bit_cast<int32_t>(readWord(address));
}

grace::Words readWords(uint64_t address)
{
    const auto* bytes = checkedBytes(address, 20);
    return { word(bytes), word(bytes + 4), word(bytes + 8), word(bytes + 12), word(bytes + 16) };
}

void writeMinusOne(uint8_t* bytes) noexcept
{
    bytes[0] = bytes[1] = bytes[2] = bytes[3] = 0xFF;
}

void diagnostic(const char* reason) noexcept
{
    const auto count = ++malformed;
    try
    {
        if (count <= 8)
        {
            REXLOG_WARN("Capital grace: unsupported native input reason={} count={} token_imbalances={}", reason, count, tokens().Imbalances());
        }
    }
    catch (...)
    {
    }
}

bool supportedProfile() noexcept
{
    try
    {
        for (const auto& expected : kProfile)
        {
            if (readWord(expected.address) != expected.value)
            {
                throw std::runtime_error("capital grace: native instruction mismatch");
            }
        }
        return true;
    }
    catch (...)
    {
        try
        {
            if (!unavailableWarning.exchange(true))
            {
                REXLOG_WARN("Capital grace is unavailable: supported native hook profile could not be confirmed");
            }
        }
        catch (...)
        {
        }
        return false;
    }
}

bool active(int32_t& year, int32_t& barbarian)
{
    if (!rerevved::raging_barbarians::Evaluate(0).enabled)
    {
        return false;
    }
    year = signedWord(kYear);
    if (!grace::Active(true, year) || !supportedProfile())
    {
        return false;
    }
    barbarian = signedWord(kBarbarian);
    if (!grace::Player(barbarian))
    {
        diagnostic("Barbarian ordinal");
        return false;
    }
    return true;
}

grace::Actor readActor(uint32_t address)
{
    const auto* bytes = checkedBytes(address, 0x24);
    return { std::bit_cast<int8_t>(bytes[0]), std::bit_cast<int8_t>(bytes[1]), word(bytes + 12), { signedHalf(bytes + 0x1C), signedHalf(bytes + 0x1E) }, signedHalf(bytes + 0x20) };
}

grace::City readCity(int32_t ordinal)
{
    const auto address = grace::CityAddress(ordinal);
    if (!address)
    {
        throw std::runtime_error("capital grace: invalid city ordinal");
    }
    const auto* bytes = checkedBytes(*address, 0x34);
    return { std::bit_cast<int8_t>(bytes[0]), std::bit_cast<int8_t>(bytes[3]), word(bytes + 0x10), { signedHalf(bytes + 0x30), signedHalf(bytes + 0x32) } };
}

bool protectedCity(int32_t ordinal, int32_t barbarian, std::optional<grace::Point> target = std::nullopt)
{
    const auto city = readCity(ordinal);
    if (!grace::Player(city.owner))
    {
        diagnostic("city owner");
        return false;
    }
    return grace::ProtectedCity(city, barbarian, target);
}

void trace(const grace::Words& words, int32_t slot, grace::Point target, int32_t city, int32_t year, bool busy, bool token, bool special) noexcept
{
    const auto count = ++refusals;
    try
    {
        if (REXCVAR_GET(raging_barbarians_trace))
        {
            REXLOG_INFO("Capital grace: refused={} command={} P={} K={} target=({}, {}) city={} year={} fresh={} busy={} token_refused={} token_imbalances={} token_capacity_failures={} special={}",
                        count,
                        words[0],
                        words[1],
                        slot,
                        target.x,
                        target.y,
                        city,
                        year,
                        slot >= 0 && !busy,
                        busy,
                        token,
                        tokens().Imbalances(),
                        tokens().CapacityFailures(),
                        special);
        }
    }
    catch (...)
    {
    }
}

bool refuseActor(const grace::Words& words, uint32_t record, uint32_t address, const grace::Actor& actor, int32_t slot, grace::Point target, int32_t city, int32_t year, bool special)
{
    const auto        player = static_cast<int32_t>(words[1]);
    grace::Retirement plan{};
    uint8_t*          budget    = nullptr;
    uint8_t*          pending   = nullptr;
    uint8_t*          selection = nullptr;
    uint8_t*          cache     = nullptr;
    // Producers set 0x8000 before submitting. A consumed native no-action exit
    // clears it (0x82CDFA68). Left set, 0x82D26CA4 waits on the unit forever.
    uint8_t* const flags = checkedBytes(address + 0xC, 4, true);
    if ((actor.flags & 1) == 0)
    {
        plan = grace::PlanRetirement(actor.flags, player, slot, signedWord(kCurrentPlayer), signedWord(kSelected), signedWord(kCache + 4 * player));
        // Resolve every write before changing any byte. No guest call may intervene.
        budget  = checkedBytes(address + 2, 1, true);
        pending = checkedBytes(address + 0x22, 2, true);
        if (plan.selection)
        {
            selection = checkedBytes(kSelected, 4, true);
        }
        if (plan.cache)
        {
            cache = checkedBytes(kCache + 4 * player, 4, true);
        }
    }
    flags[2] &= 0x7F;
    if (plan.actor)
    {
        *budget    = 0;
        pending[0] = pending[1] = 0xFF;
        if (selection)
        {
            writeMinusOne(selection);
        }
        if (cache)
        {
            writeMinusOne(cache);
        }
    }
    const bool matched = tokens().Refuse(record, words);
    trace(words, slot, target, city, year, (actor.flags & 1) != 0, matched, special);
    return true;
}

void beginProducer(grace::Callsite site, PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra) noexcept
{
    if (!tokens().PushProducer(frame.u32, site, { command.u32, player.u32, slot.u32, direction.u32, extra.u32 }))
    {
        diagnostic("producer stack capacity/frame");
    }
}

} // namespace

bool GuardCapitalGraceAdmission(PPCRegister& record, PPCRegister& command, PPCRegister& player, PPCRegister& argument, PPCRegister& direction)
{
    if (auto* thread = rex::runtime::ThreadState::Get())
    {
        grace::BeginSpecialDispatch(thread->context()->r1.u32);
    }
    try
    {
        int32_t year      = 0;
        int32_t barbarian = -1;
        if ((command.u32 != 0 && command.u32 != 1 && command.u32 != 17 && command.u32 != 27) || !active(year, barbarian))
        {
            return false;
        }
        const auto words = readWords(record.u32);
        if (words[0] != command.u32 || words[1] != player.u32 || words[2] != argument.u32 || words[3] != direction.u32)
        {
            diagnostic("dispatcher record/register mismatch");
            return false;
        }
        if (player.s32 != barbarian)
        {
            return false;
        }
        if (command.u32 == 27)
        {
            if (!protectedCity(argument.s32, barbarian))
            {
                return false;
            }
            const bool matched = tokens().Refuse(record.u32, words);
            trace(words, -1, { -1, -1 }, argument.s32, year, false, matched, false);
            return true;
        }
        const auto slot    = grace::ActorSlot(words);
        const auto address = slot ? grace::UnitAddress(player.s32, *slot) : std::nullopt;
        if (!address)
        {
            diagnostic("actor index");
            return false;
        }
        const auto actor = readActor(*address);
        if (!grace::LiveActor(actor, player.s32, barbarian))
        {
            diagnostic("actor owner/type/liveness");
            return false;
        }
        const auto    definition = readWord(grace::kDefinitions + grace::kDefinitionStride * actor.type + 0x50);
        const int32_t width      = signedWord(0x82F77364);
        const int32_t height     = signedWord(0x82F77448);
        if (!grace::Adjacent(actor.position, width, height, 0, 0))
        {
            diagnostic("actor coordinates/map dimensions");
            return false;
        }
        if (command.u32 == 17 && ((definition & 0x20000) || actor.type == 24))
        {
            const auto route = (definition & 0x20000) ? grace::SpecialRoute::Nuclear : grace::SpecialRoute::Type24;
            if (grace::AdmitSpecial({ route, words, actor, year, barbarian }) == grace::SpecialDecision::Native)
            {
                return false;
            }
            return refuseActor(words, record.u32, *address, actor, *slot, { -1, -1 }, -1, year, true);
        }
        const auto step = grace::Direction(words, actor.direction);
        if (!step)
        {
            diagnostic("command direction");
            return false;
        }
        const auto target = grace::Adjacent(actor.position, width, height, signedWord(0x82F7A0B8 + 4 * *step), signedWord(0x82F7A168 + 4 * *step));
        if (!target)
        {
            diagnostic("adjacent target");
            return false;
        }
        const uint32_t index = 40 * target->x + target->y;
        if (command.u32 == 17 && (actor.type == 28 || actor.type == 29))
        {
            const auto flags = half(checkedBytes(0x8312A608 + 2 * index, 2));
            const auto owner = std::bit_cast<int8_t>(*checkedBytes(0x830E6850 + index, 1));
            if ((flags & 1) && !grace::Player(owner))
            {
                diagnostic("foreign-city owner plane");
                return false;
            }
            if (grace::SpecialArrival(command.u32, actor.type, flags, owner, player.s32))
            {
                return false;
            }
        }
        const auto city = std::bit_cast<int8_t>(*checkedBytes(0x83128D08 + index, 1));
        if (city < 0 || !protectedCity(city, barbarian, *target))
        {
            return false;
        }
        return refuseActor(words, record.u32, *address, actor, *slot, *target, city, year, false);
    }
    catch (...)
    {
        diagnostic("admission memory/profile");
        return false;
    }
}

bool GuardCapitalGraceCapture(PPCRegister& city, PPCRegister& newOwner)
{
    try
    {
        int32_t year      = 0;
        int32_t barbarian = -1;
        if (!active(year, barbarian) || newOwner.s32 != barbarian || !protectedCity(city.s32, barbarian))
        {
            return false;
        }
        trace({ 27, newOwner.u32, city.u32, 0, 0 }, -1, { -1, -1 }, city.s32, year, false, false, false);
        return true;
    }
    catch (...)
    {
        diagnostic("capture memory/profile");
        return false;
    }
}

void BeginCapitalGraceSubmit(PPCRegister& frame, PPCRegister& queue, PPCRegister& ring)
{
    if (queue.u32 == kQueue && ring.u32 == kRing)
    {
        tokens().BeginSubmit(frame.u32, queue.u32);
        if (const auto* observer = queueObserver())
        {
            observer->beginSubmit(frame.u32, queue.u32);
        }
    }
    else
    {
        tokens().Reset();
        diagnostic("queue identity");
    }
}

void AppendCapitalGraceToken(PPCRegister& frame, PPCRegister& queue, PPCRegister& record)
{
    try
    {
        if (queue.u32 == kQueue)
        {
            const auto write = readWord(kQueue - 4);
            const auto read  = readWord(kQueue);
            if (write < read || write - read >= 32 || write == UINT32_MAX)
            {
                diagnostic("queue append occupancy");
            }
            const auto words = readWords(record.u32);
            tokens().Append(frame.u32, record.u32, words, write, read);
            if (const auto* observer = queueObserver())
            {
                observer->append(frame.u32, queue.u32, record.u32, words, write, read);
            }
        }
    }
    catch (...)
    {
        tokens().Reset();
        diagnostic("queue append");
    }
}

void CompleteCapitalGraceAppend(PPCRegister& frame)
{
    tokens().CompleteAppend(frame.u32);
    if (const auto* observer = queueObserver())
    {
        observer->completeAppend(frame.u32);
    }
}

void ObserveCapitalGraceDrainSerial(PPCRegister& frame, PPCRegister& queue, PPCRegister& serial)
{
    try
    {
        if (queue.u32 == kQueue)
        {
            const auto write = readWord(kQueue - 4);
            if (write <= serial.u32 || write - serial.u32 > 32)
            {
                diagnostic("queue drain occupancy");
            }
            tokens().DrainSerial(frame.u32, serial.u32, write);
            if (const auto* observer = queueObserver())
            {
                observer->drainSerial(frame.u32, queue.u32, serial.u32, write);
            }
        }
    }
    catch (...)
    {
        tokens().Reset();
        diagnostic("queue drain serial");
    }
}

void BindCapitalGraceDrain(PPCRegister& frame)
{
    try
    {
        const auto words = readWords(uint64_t{ frame.u32 } + 80);
        tokens().DrainRecord(frame.u32, frame.u32 + 80, words);
        if (const auto* observer = queueObserver())
        {
            observer->drainRecord(frame.u32, frame.u32 + 80, words);
        }
    }
    catch (...)
    {
        tokens().Reset();
        diagnostic("queue drain record");
    }
}

void EndCapitalGraceSubmit(PPCRegister& frame)
{
    tokens().EndSubmit(frame.u32);
    if (const auto* observer = queueObserver())
    {
        observer->endSubmit(frame.u32);
    }
}

void BeginCapitalGraceBlitz(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra)
{
    beginProducer(grace::Callsite::Blitz, frame, command, player, slot, direction, extra);
}

void BeginCapitalGraceOrdinary(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra)
{
    beginProducer(grace::Callsite::Ordinary, frame, command, player, slot, direction, extra);
}

void BeginCapitalGracePacked(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra)
{
    beginProducer(grace::Callsite::Packed, frame, command, player, slot, direction, extra);
}

void BeginCapitalGraceLinked(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra)
{
    beginProducer(grace::Callsite::Linked, frame, command, player, slot, direction, extra);
}

bool EndCapitalGraceBlitz(PPCRegister& frame)
{
    return tokens().PopProducer(frame.u32, grace::Callsite::Blitz).refused;
}

bool EndCapitalGraceOrdinary(PPCRegister& frame)
{
    return tokens().PopProducer(frame.u32, grace::Callsite::Ordinary).refused;
}

bool EndCapitalGracePacked(PPCRegister& frame)
{
    return tokens().PopProducer(frame.u32, grace::Callsite::Packed).refused;
}

void PrepareCapitalGraceParent(PPCRegister& frame, PPCRegister& units, PPCRegister& globals, PPCRegister& parent, PPCRegister& selected, PPCRegister& oldFlags)
{
    try
    {
        if (units.u32 != grace::kUnits || globals.u32 != 0x83130000 || parent.u32 != selected.u32 || (oldFlags.u32 & 1))
        {
            return;
        }
        const auto player  = signedWord(kCurrentPlayer);
        const auto address = grace::UnitAddress(player, parent.s32);
        if (!address || signedWord(kSelected) != parent.s32)
        {
            return;
        }
        const auto actor = readActor(*address);
        if (actor.owner != player || (actor.flags & 0x80000001u) || actor.flags != oldFlags.u32)
        {
            return;
        }
        checkedBytes(*address + 12, 4, true);
        checkedBytes(frame.u32, 0x390);
        tokens().PrepareParent(frame.u32, player, parent.s32);
    }
    catch (...)
    {
        diagnostic("parent preparation");
    }
}

bool EndCapitalGraceLinked(PPCRegister& frame, PPCRegister& units, PPCRegister& globals, PPCRegister& parent, PPCRegister& selected)
{
    const auto submission = tokens().PopProducer(frame.u32, grace::Callsite::Linked);
    try
    {
        if (!submission.refused || units.u32 != grace::kUnits || globals.u32 != 0x83130000 || parent.u32 != selected.u32 || parent.s32 != submission.parentSlot || submission.childSlot == submission.parentSlot || signedWord(kCurrentPlayer) != submission.parentPlayer || signedWord(kSelected) != parent.s32)
        {
            return false;
        }
        const auto address = grace::UnitAddress(submission.parentPlayer, submission.parentSlot);
        if (!address)
        {
            return false;
        }
        const auto actor = readActor(*address);
        if (actor.owner != submission.parentPlayer || (actor.flags & 0x80000000u) || !(actor.flags & 1))
        {
            return false;
        }
        checkedBytes(*address + 12, 4, true);
        checkedBytes(frame.u32, 0x390);
        return tokens().ArmCleanup(frame.u32, submission);
    }
    catch (...)
    {
        diagnostic("linked cleanup guard");
        return false;
    }
}

bool FinishCapitalGraceParent(PPCRegister& frame)
{
    return tokens().FinishCleanup(frame.u32);
}
