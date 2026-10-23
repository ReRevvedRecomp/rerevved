#include "completion_probe.h"

#include "guest_memory.h"
#include "resource_mirror_registry.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

REXCVAR_DEFINE_BOOL(native_completion_probe, false, "ReRevved", "Observe completion accounting, sampled slots, queues and allocation routes; cp_* timing fields and Native completion probe records")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::uint32_t              kGlobal      = 0x820009F0;
constexpr std::uint32_t              kExtentBegin = 0x1EBB0000, kExtentEnd = 0x1F1B0000;
constexpr std::array<std::size_t, 5> kSlots = { 5, 6, 7, 30, 31 };

bool readGuest(void*, std::uint32_t address, std::uint8_t* bytes, std::size_t size)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address || !size || size - 1 > UINT32_MAX - address)
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + static_cast<std::uint32_t>(size - 1);
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    return CopyGuestGuarded(bytes, memory->TranslateVirtual<const std::uint8_t*>(address), size);
}

std::optional<std::uint32_t> readWord(std::uint32_t base, std::uint32_t offset = 0)
{
    return ReadCompletionWord(true, readGuest, nullptr, base, offset);
}

std::uint32_t currentDevice()
{
    const auto global = readWord(kGlobal);
    if (!global)
    {
        PublishedCompletionProbe().Failure();
        return 0;
    }
    if (!*global)
        return 0;
    const auto device = readWord(*global);
    if (!device)
        PublishedCompletionProbe().Failure();
    return device.value_or(0);
}

void failure() noexcept
{
    try
    {
        PublishedCompletionProbe().Failure();
    }
    catch (...)
    {
    }
}

thread_local std::uint32_t pendingStore = 0;

bool complete(const CompletionSnapshot& snapshot)
{
    return snapshot.unreadable == 0 && snapshot.Has(4) && snapshot.Has(15) && snapshot.words[15] == 0;
}

} // namespace

std::optional<std::uint32_t> ReadCompletionWord(bool enabled, CompletionReader reader, void* context, std::uint32_t base, std::uint32_t offset) noexcept
{
    if (!enabled)
        return std::nullopt;
    if (!reader || !base || offset > UINT32_MAX - base || base + offset > UINT32_MAX - 3)
        return std::nullopt;
    std::array<std::uint8_t, 4> bytes{};
    try
    {
        if (!reader(context, base + offset, bytes.data(), bytes.size()))
            return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

CompletionSnapshot ReadCompletionSnapshot(CompletionReader reader, void* context, std::uint32_t device) noexcept
{
    CompletionSnapshot snapshot;
    if (!device)
        return snapshot;
    const auto word = [&](std::size_t index, std::uint32_t base, std::uint32_t offset)
    {
        const auto value = ReadCompletionWord(true, reader, context, base, offset);
        if (value)
        {
            snapshot.words[index] = *value;
            snapshot.valid |= std::uint64_t{ 1 } << index;
        }
        else
            ++snapshot.unreadable;
    };
    for (std::size_t i = 0; i < kCompletionOffsets.size(); ++i)
        word(i, device, kCompletionOffsets[i]);
    // Null chains are absent before initialization, never dereferenced or invented as C=0.
    if (snapshot.Has(2) && snapshot.words[2])
        word(29, snapshot.words[2], 0);
    if (snapshot.Has(3) && snapshot.words[3])
    {
        word(30, snapshot.words[3], 0x10);
        word(31, snapshot.words[3], 0x14);
    }
    return snapshot;
}

std::uint32_t CompletionResidual(std::uint32_t before, std::uint32_t after, std::uint64_t additions, std::uint64_t retirements) noexcept
{
    return after - before - static_cast<std::uint32_t>(additions) + static_cast<std::uint32_t>(retirements);
}

CompletionProbe::Stamp CompletionProbe::Sample()
{
    std::lock_guard lock(mutex_);
    return { version_, quality_, stores_ };
}

void CompletionProbe::Lost()
{
    ++report_.counts.eventDrop;
    ++quality_;
}

CompletionProbe::Device* CompletionProbe::FindDevice(std::uint32_t address)
{
    if (!address)
    {
        ++report_.counts.epochAmbiguous;
        return &unknownDevice_;
    }
    for (auto& device : devices_)
        if (device.address == address)
            return &device;
    for (auto& device : devices_)
        if (!device.address)
        {
            device.address = address;
            return &device;
        }
    Lost();
    ++report_.counts.epochAmbiguous;
    return nullptr;
}

void CompletionProbe::Record(CompletionRecord record)
{
    record.sequence = ++sequence_;
    if (report_.size == report_.records.size())
    {
        Lost();
        return;
    }
    report_.records[report_.size++] = record;
}

void CompletionProbe::SampleFields(Device& device, CompletionRecord& record)
{
    const auto& now    = record.snapshot;
    const auto& old    = device.previous;
    auto&       counts = report_.counts;
    counts.unreadable += now.unreadable;
    if (now.unreadable)
        ++quality_;
    if (now.Has(0))
    {
        ++((now.words[0] & 1) ? counts.pOdd : counts.pEven);
        counts.pZero += now.words[0] == 0;
    }
    if (now.Has(1))
    {
        counts.maskNonzero += now.words[1] != 0;
        counts.maskChange += old.Has(1) && old.words[1] != now.words[1];
    }
    counts.s2Change += now.Has(3) && old.Has(3) && now.words[3] != old.words[3];
    for (std::size_t i = 0; i < kSlots.size(); ++i)
    {
        const auto slot = kSlots[i];
        counts.slotSamples[i] += now.Has(slot);
        counts.slotNonzero[i] += now.Has(slot) && now.words[slot] != 0;
        // Replacing S2 changes the sampled slot identity as well as its value.
        counts.slotChange[i] += now.Has(slot) && old.Has(slot) &&
                                (now.words[slot] != old.words[slot] || (slot >= 30 && now.words[3] != old.words[3]));
    }
    for (std::size_t i = 9; i <= 11; ++i)
    {
        counts.queueNonzero += now.Has(i) && now.words[i] != 0;
        if (now.Has(i) && old.Has(i) && now.words[i] != old.words[i])
            ++(i == 9 ? counts.queueSegmentChange : counts.queueCursorChange);
    }
    record.epoch        = device.epoch;
    record.additions    = device.additions;
    record.retirements  = device.d;
    record.previousCall = device.previousCall;
    device.previousCall = record.call;
    device.previous     = now;
}

std::uint64_t CompletionProbe::Begin(std::uint32_t pc, std::uint32_t address, std::uint32_t thread, const CompletionSnapshot& snapshot, Stamp stamp, const std::array<std::uint32_t, 12>& payload, std::uint32_t payloadValid)
{
    std::lock_guard lock(mutex_);
    auto*           device = FindDevice(address);
    if (!device)
        return 0;
    if (pc == 0x8269C8E0)
    {
        ++device->epoch;
        device->previous   = {};
        device->exit.valid = false;
        ++version_;
    }
    Call* free    = nullptr;
    bool  overlap = false;
    for (auto& call : calls_)
    {
        if (!call.id && !free)
            free = &call;
        if (call.id && call.device == address &&
            (pc == 0x8269C810 || pc == 0x826AC218) &&
            (call.pc == 0x8269C810 || call.pc == 0x826AC218))
        {
            call.uncertain = true;
            overlap        = true;
        }
    }
    if (!free)
    {
        Lost();
        ++report_.counts.pairError;
        return 0;
    }
    const auto       id = ++nextCall_;
    CompletionRecord record;
    record.device       = address;
    record.thread       = thread;
    record.pc           = pc;
    record.call         = id;
    record.kind         = "enter";
    record.snapshot     = snapshot;
    record.payload      = payload;
    record.payloadValid = payloadValid;
    SampleFields(*device, record);
    Record(record);
    *free = { id, pc, address, thread, { snapshot, device->additions, device->d, device->epoch, stamp.quality, id, complete(snapshot) }, overlap || stamp.stores || stores_ || stamp.version != version_ || stamp.quality != quality_ };
    if (pc == 0x8269C810)
        ++report_.counts.c810Enter;
    return id;
}

void CompletionProbe::Balance(Device& device, const Boundary& before, CompletionRecord& record, bool uncertain)
{
    const auto additions = device.additions - before.additions;
    const auto d         = device.d - before.d;
    const auto residual  = CompletionResidual(before.snapshot.words[4], record.snapshot.words[4], additions, d);
    record.payload[0]    = before.snapshot.words[4];
    record.payload[1]    = record.snapshot.words[4];
    record.payload[2]    = static_cast<std::uint32_t>(additions);
    record.payload[3]    = static_cast<std::uint32_t>(d);
    record.payload[4]    = residual;
    record.payload[5]    = static_cast<std::uint32_t>(before.call);
    if (!before.snapshot.Has(4))
        record.payloadValid &= ~((1U << 0) | (1U << 4));
    if (!record.snapshot.Has(4))
        record.payloadValid &= ~((1U << 1) | (1U << 4));
    record.previousCall = before.call;
    if (before.epoch != device.epoch)
        ++report_.counts.epochAmbiguous;
    if (uncertain || !before.valid || !complete(record.snapshot) || before.epoch != device.epoch || before.quality != quality_ || report_.size == report_.records.size())
    {
        record.balance = CompletionBalance::Indeterminate;
        ++report_.counts.balanceIndeterminate;
    }
    else if (residual)
    {
        record.balance = CompletionBalance::Bad;
        ++report_.counts.balanceBad;
    }
    else
    {
        record.balance = CompletionBalance::Ok;
        ++report_.counts.balanceOk;
    }
    Record(record);
}

void CompletionProbe::End(std::uint64_t id, const CompletionSnapshot& snapshot, Stamp stamp, std::uint32_t result)
{
    std::lock_guard lock(mutex_);
    auto            found = std::find_if(calls_.begin(), calls_.end(), [id](const Call& call)
                                         {
                                  return id && call.id == id;
                                         });
    if (found == calls_.end())
    {
        ++report_.counts.pairError;
        ++quality_;
        return;
    }
    const auto call = *found;
    found->id       = 0;
    auto* device    = FindDevice(call.device);
    if (!device)
        return;
    CompletionRecord record;
    record.device      = call.device;
    record.thread      = call.thread;
    record.pc          = call.pc;
    record.call        = id;
    record.kind        = "exit";
    record.snapshot    = snapshot;
    record.payload[10] = call.stubbed;
    record.payload[11] = result;
    SampleFields(*device, record);
    const bool uncertain = call.uncertain || stamp.stores || stores_ || stamp.version != version_ || stamp.quality != quality_;
    if (call.pc == 0x8269C810 || call.pc == 0x826AC218)
        Balance(*device, call.start, record, uncertain);
    else
        Record(record);
    if (call.pc == 0x826AC218)
        report_.counts.dropped += result != 0;
    if (call.pc == 0x8269C8E0)
    {
        ++(result == 0 ? report_.counts.initSuccess : report_.counts.initFailure);
        if (call.start.epoch != device->epoch)
            ++report_.counts.epochAmbiguous;
    }
    if (call.pc == 0x8269C810)
    {
        ++report_.counts.c810Exit;
        report_.counts.c810ExitNNonzero += snapshot.Has(4) && snapshot.words[4] != 0;
        if (device->exit.valid)
        {
            record.kind = "carry";
            Balance(*device, device->exit, record, uncertain);
        }
        device->exit = { snapshot, device->additions, device->d, device->epoch, quality_, id, !uncertain && complete(snapshot) && quality_ == stamp.quality };
    }
}

void CompletionProbe::Observe(std::uint32_t pc, std::uint32_t address, std::uint32_t thread, const CompletionSnapshot& snapshot, const std::array<std::uint32_t, 12>& payload, bool epoch, const char* kind)
{
    std::lock_guard lock(mutex_);
    auto*           device = FindDevice(address);
    if (!device)
    {
        report_.counts.unreadable += snapshot.unreadable;
        return;
    }
    if (epoch)
    {
        ++device->epoch;
        device->previous   = {};
        device->exit.valid = false;
        ++version_;
    }
    CompletionRecord record;
    record.pc       = pc;
    record.device   = address;
    record.thread   = thread;
    record.kind     = kind;
    record.snapshot = snapshot;
    record.payload  = payload;
    for (const auto& call : calls_)
        if (call.id && call.device == address)
            record.call = std::max(record.call, call.id);
    if (kind == std::string_view("callback"))
        ++report_.counts.callbacks;
    SampleFields(*device, record);
    Record(record);
}

void CompletionProbe::StoreBegin()
{
    std::lock_guard lock(mutex_);
    ++stores_;
    ++version_;
}

void CompletionProbe::Stubbed(std::uint64_t id)
{
    std::lock_guard lock(mutex_);
    for (auto& call : calls_)
        if (call.id == id && id)
            call.stubbed = true;
}

void CompletionProbe::StoreCancel()
{
    std::lock_guard lock(mutex_);
    if (stores_)
        --stores_;
    else
    {
        ++report_.counts.pairError;
        ++quality_;
    }
    ++version_;
}

void CompletionProbe::StoreEnd(std::uint32_t pc, std::uint32_t address, std::uint32_t thread, CompletionAdd path, std::uint32_t amount, std::optional<std::uint32_t> stored, bool retirement, const CompletionSnapshot& snapshot)
{
    std::lock_guard lock(mutex_);
    if (stores_)
        --stores_;
    else
    {
        ++report_.counts.pairError;
        ++quality_;
    }
    ++version_;
    auto* device = FindDevice(address);
    if (!device)
        return;
    if (retirement)
    {
        ++device->d;
        ++report_.counts.d;
    }
    else
    {
        device->additions += amount;
        report_.counts.adds[static_cast<std::size_t>(path)] += amount;
        ++report_.counts.addCalls[static_cast<std::size_t>(path)];
    }
    if (!stored)
    {
        ++report_.counts.unreadable;
        ++quality_;
    }
    CompletionRecord record;
    record.pc          = pc;
    record.device      = address;
    record.thread      = thread;
    record.epoch       = device->epoch;
    record.kind        = retirement ? "retire" : "add";
    record.additions   = device->additions;
    record.retirements = device->d;
    record.snapshot    = snapshot;
    report_.counts.unreadable += snapshot.unreadable;
    if (snapshot.unreadable)
        ++quality_;
    record.payload[0] = amount;
    record.payload[1] = stored.value_or(0);
    record.payload[2] = stored.has_value();
    record.payload[3] = static_cast<std::uint32_t>(path);
    if (!stored)
        record.payloadValid &= ~(1U << 1);
    // Associate by device across threads; ambiguous overlap stays explicit.
    for (const auto& call : calls_)
        if (call.id && call.device == address && (call.pc == 0x8269BD98 || call.pc == 0x826AC218 || call.pc == 0x8269C810))
        {
            if (record.call)
            {
                record.call = 0;
                break;
            }
            record.call = call.id;
        }
    Record(record);
}

void CompletionProbe::Reset(std::uint32_t address)
{
    std::lock_guard lock(mutex_);
    ++report_.counts.watchdogReset;
    ++quality_;
    ++version_;
    if (auto* device = FindDevice(address))
    {
        device->exit.valid = false;
        CompletionRecord record;
        record.kind   = "watchdog_reset";
        record.device = address;
        record.pc     = 0x826A5BC8;
        record.epoch  = device->epoch;
        Record(record);
    }
}

void CompletionProbe::Failure()
{
    std::lock_guard lock(mutex_);
    ++report_.counts.unreadable;
    ++quality_;
}

void CompletionProbe::AllocationBegin(std::uint32_t thread, std::uint32_t pc, std::uint32_t device, std::uint32_t caller, std::optional<std::uint32_t> request, std::uint32_t alignment, std::uint32_t manager, std::uint32_t flags, std::uint32_t requestPointer)
{
    std::lock_guard lock(mutex_);
    if (allocationPairingLost_)
    {
        Lost();
        return;
    }
    if (!request)
    {
        ++report_.counts.unreadable;
        ++quality_;
    }
    for (auto& allocation : allocations_)
        if (!allocation.active)
        {
            allocation                = {};
            allocation.active         = true;
            allocation.thread         = thread;
            allocation.pc             = pc;
            allocation.device         = device;
            allocation.caller         = caller;
            allocation.request        = request;
            allocation.alignment      = alignment;
            allocation.manager        = manager;
            allocation.flags          = flags;
            allocation.id             = ++nextCall_;
            allocation.requestPointer = requestPointer;
            return;
        }
    Lost();
    ++report_.counts.pairError;
    allocationPairingLost_ = true;
}

void CompletionProbe::AllocationTarget(std::uint32_t thread, std::uint32_t target, std::uint32_t context, std::optional<std::uint32_t> adjusted, const std::array<std::optional<std::uint32_t>, 3>& managerWords)
{
    std::lock_guard lock(mutex_);
    if (allocationPairingLost_)
        return;
    Allocation* top = nullptr;
    for (auto& allocation : allocations_)
        if (allocation.active && allocation.thread == thread && (!top || allocation.id > top->id))
            top = &allocation;
    if (!top || top->pc != 0x826A6AB8)
    {
        ++report_.counts.pairError;
        ++quality_;
        return;
    }
    top->target           = target;
    top->context          = context;
    top->adjusted         = adjusted;
    top->managerWords     = managerWords;
    const auto unreadable = static_cast<std::uint64_t>(!adjusted) +
                            static_cast<std::uint64_t>(std::count_if(managerWords.begin(), managerWords.end(), [](const auto& value)
                                                                     {
                                                                         return !value;
                                                                     }));
    if (unreadable)
    {
        report_.counts.unreadable += unreadable;
        ++quality_;
    }
}

void CompletionProbe::AllocationEnd(std::uint32_t thread, std::uint32_t pc, std::uint32_t result, std::uint32_t physical, std::uint32_t regionBase, std::uint32_t regionSize)
{
    std::lock_guard lock(mutex_);
    if (allocationPairingLost_)
        return;
    Allocation* top = nullptr;
    for (auto& allocation : allocations_)
        if (allocation.active && allocation.thread == thread && (!top || allocation.id > top->id))
            top = &allocation;
    const auto entry = pc == 0x8269B8EC ? 0x8269B81C : 0x826A6AB8;
    if (!top || top->pc != entry)
    {
        ++report_.counts.pairError;
        ++quality_;
        return;
    }
    const auto allocation = *top;
    top->active           = false;
    // Preserve the actual manager route on its outer B818 call too.
    Allocation* parent = nullptr;
    for (auto& outer : allocations_)
        if (outer.active && outer.thread == thread && outer.id < allocation.id)
            if (!parent || outer.id > parent->id)
                parent = &outer;
    if (parent)
    {
        parent->target         = allocation.target;
        parent->context        = allocation.context;
        parent->manager        = allocation.manager;
        parent->adjusted       = allocation.adjusted;
        parent->managerWords   = allocation.managerWords;
        parent->managerRequest = allocation.request;
        parent->requestPointer = allocation.requestPointer;
        parent->flags          = allocation.flags;
    }
    if (physical < kExtentBegin || physical >= kExtentEnd)
        return;
    ++report_.counts.extentHit;
    if (!regionSize)
    {
        ++report_.counts.unreadable;
        ++quality_;
    }
    // A hit proves ownership only when this observed request returned the whole region.
    const bool owner = regionBase == result && regionSize == kExtentEnd - kExtentBegin && allocation.adjusted && *allocation.adjusted == regionSize;
    // Wait for the outer return so the first chain includes the original dword request.
    if (parent && parent->pc == 0x8269B81C)
        return;
    report_.counts.extentOwnerJoined += owner;
    auto*      device = FindDevice(allocation.device);
    const auto epoch  = device ? device->epoch : 0;
    if (!owner && std::any_of(extentGenerations_.begin(), extentGenerations_.end(), [&](const ExtentIdentity& identity)
                              {
                                  return identity.used && identity.device == allocation.device && identity.base == regionBase && identity.size == regionSize && identity.epoch == epoch;
                              }))
        return;
    auto free = std::find_if(extentGenerations_.begin(), extentGenerations_.end(), [](const ExtentIdentity& identity)
                             {
                                 return !identity.used;
                             });
    if (free == extentGenerations_.end())
    {
        Lost();
        return;
    }
    *free = { allocation.device, regionBase, regionSize, epoch, true };
    CompletionRecord record;
    record.kind    = "allocation";
    record.pc      = pc;
    record.call    = allocation.id;
    record.thread  = thread;
    record.device  = allocation.device;
    record.epoch   = epoch;
    record.payload = { allocation.caller, allocation.request.value_or(0), allocation.adjusted.value_or(0), allocation.alignment, result, physical, regionBase, regionSize, allocation.manager, allocation.target, allocation.context, owner ? 1U : 0U };
    if (!allocation.request)
        record.payloadValid &= ~(1U << 1);
    if (!allocation.adjusted)
        record.payloadValid &= ~(1U << 2);
    if (!regionSize)
        record.payloadValid &= ~((1U << 6) | (1U << 7));
    if (!allocation.target)
        record.payloadValid &= ~((1U << 9) | (1U << 10));
    if (allocation.request)
    {
        record.snapshot.words[3] = allocation.pc == 0x8269B81C ? *allocation.request << 2 : *allocation.request;
        record.snapshot.valid |= std::uint64_t{ 1 } << 3;
    }
    record.snapshot.words[4] = allocation.flags;
    record.snapshot.valid |= std::uint64_t{ 1 } << 4;
    record.snapshot.words[5] = allocation.adjusted.has_value();
    record.snapshot.valid |= std::uint64_t{ 1 } << 5;
    record.snapshot.words[6] = allocation.requestPointer;
    record.snapshot.valid |= std::uint64_t{ 1 } << 6;
    const auto managerRequest = allocation.pc == 0x8269B81C ? allocation.managerRequest : allocation.request;
    if (managerRequest)
    {
        record.snapshot.words[7] = *managerRequest;
        record.snapshot.valid |= std::uint64_t{ 1 } << 7;
    }
    for (std::size_t i = 0; i < allocation.managerWords.size(); ++i)
        if (allocation.managerWords[i])
        {
            record.snapshot.words[i] = *allocation.managerWords[i];
            record.snapshot.valid |= std::uint64_t{ 1 } << i;
        }
    Record(record);
}

CompletionReport CompletionProbe::Take()
{
    std::lock_guard lock(mutex_);
    auto            result = report_;
    for (const auto& call : calls_)
        result.counts.unmatched += call.id != 0;
    for (const auto& allocation : allocations_)
        result.counts.unmatched += allocation.active;
    result.counts.unmatched += stores_;
    report_ = {};
    return result;
}

std::atomic<std::int8_t> completionProbeState{ -1 };

bool RefreshCompletionProbeRequested() noexcept
{
    const bool requested = REXCVAR_GET(native_completion_probe);
    completionProbeState.store(requested ? 1 : 0, std::memory_order_relaxed);
    return requested;
}

CompletionProbe& PublishedCompletionProbe() noexcept
{
    static CompletionProbe probe;
    return probe;
}

std::uint32_t CompletionThread() noexcept
{
    if (!CompletionProbeRequested())
        return 0;
    const auto* thread = rex::system::XThread::GetCurrentThread();
    return thread ? thread->thread_id() : 0;
}

void CompletionScope::Begin(std::uint32_t pc) noexcept
{
    const auto& context = context_;
    try
    {
        device_                                    = pc == 0x826AC020 ? currentDevice() : context.r3.u32;
        auto&                         probe        = PublishedCompletionProbe();
        const auto                    stamp        = probe.Sample();
        const auto                    snapshot     = ReadCompletionSnapshot(readGuest, nullptr, device_);
        std::array<std::uint32_t, 12> payload      = { static_cast<std::uint32_t>(context.lr), context.r4.u32, context.r5.u32, context.r6.u32, context.r7.u32, context.r8.u32, context.r3.u32 };
        std::uint32_t                 payloadValid = pc == 0x8269C8E0 ? 0x7F : 0xFFF;
        if (pc == 0x8269C8E0 && context.r4.u32)
            for (std::size_t i = 0; i < 5; ++i)
            {
                const auto value = readWord(context.r4.u32, static_cast<std::uint32_t>(i * 4 + 4));
                if (value)
                {
                    payload[7 + i] = *value;
                    payloadValid |= 1U << (7 + i);
                }
                else
                    probe.Failure();
            }
        call_ = probe.Begin(pc, device_, CompletionThread(), snapshot, stamp, payload, payloadValid);
        if (pc == 0x826AC020)
        {
            const auto cpu = ReadCompletionWord(true, readGuest, nullptr, context.r13.u32, 0x10C);
            // The selector is a byte; the high byte of this aligned BE word is that byte.
            payload[0] = context.r3.u32;
            payload[1] = cpu ? (*cpu >> 24) : UINT32_MAX;
            if (!cpu)
                probe.Failure();
            probe.Observe(pc, device_, CompletionThread(), snapshot, payload, false, "callback");
        }
    }
    catch (...)
    {
        failure();
    }
}

void CompletionScope::End() noexcept
{
    try
    {
        auto&      probe    = PublishedCompletionProbe();
        const auto stamp    = probe.Sample();
        const auto snapshot = ReadCompletionSnapshot(readGuest, nullptr, device_);
        probe.End(call_, snapshot, stamp, context_.r3.u32);
    }
    catch (...)
    {
        failure();
    }
}

void CompletionScope::Stubbed() const noexcept
{
    if (!active_ || !call_)
        return;
    try
    {
        PublishedCompletionProbe().Stubbed(call_);
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionSample(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, const std::array<std::uint32_t, 12>& payload, bool epoch, const char* kind) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        if (!device)
            device = currentDevice();
        PublishedCompletionProbe().Observe(pc, device, thread, ReadCompletionSnapshot(readGuest, nullptr, device), payload, epoch, kind);
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionStoreBegin(std::uint32_t pc) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        if (pendingStore)
        {
            failure();
            PublishedCompletionProbe().StoreCancel();
        }
        pendingStore = pc;
        PublishedCompletionProbe().StoreBegin();
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionStoreEnd(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, CompletionAdd path, std::uint32_t amount, std::uint32_t stored, bool retirement) noexcept
{
    if (!CompletionProbeRequested())
        return;
    // BE20 and BB38 are also branch targets that bypass the store.
    if (pendingStore != pc)
        return;
    pendingStore = 0;
    try
    {
        CompletionSnapshot snapshot;
        if (retirement)
            snapshot = ReadCompletionSnapshot(readGuest, nullptr, device);
        PublishedCompletionProbe().StoreEnd(pc, device, thread, path, amount, stored, retirement, snapshot);
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionStubBegin() noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        PublishedCompletionProbe().StoreBegin();
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionStubEnd(std::uint32_t device, std::uint32_t thread, std::uint32_t amount, bool success) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        if (success)
            PublishedCompletionProbe().StoreEnd(0x8269BD98, device, thread, CompletionAdd::Stub, amount, readWord(device, 0x2B00));
        else
            PublishedCompletionProbe().StoreCancel();
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionReset(std::uint32_t device) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        ObserveCompletionStoreBegin(0x826A5BC8);
        PublishedCompletionProbe().Reset(device);
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionResetEnd() noexcept
{
    if (!CompletionProbeRequested() || pendingStore != 0x826A5BC8)
        return;
    pendingStore = 0;
    try
    {
        PublishedCompletionProbe().StoreCancel();
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionCallback(std::uint32_t pc, std::uint32_t device, std::uint32_t target, std::uint32_t argument, std::size_t words) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        std::array<std::uint32_t, 12> payload = { target, argument };
        for (std::size_t i = 0; i < std::min(words, std::size_t{ 6 }); ++i)
        {
            const auto value = readWord(argument, static_cast<std::uint32_t>(i * 4));
            if (value)
            {
                payload[2 + i] = *value;
                payload[8] |= 1U << i;
            }
            else
                PublishedCompletionProbe().Failure();
        }
        ObserveCompletionSample(pc, device, CompletionThread(), payload, false, "callback");
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionAllocationBegin(std::uint32_t thread, std::uint32_t pc, std::uint32_t device, std::uint32_t caller, std::uint32_t request, std::uint32_t alignment, std::uint32_t manager, std::uint32_t flags) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        if (!device)
            device = currentDevice();
        if (pc == 0x826A6AB8)
        {
            const auto bytes = readWord(request);
            PublishedCompletionProbe().AllocationBegin(thread, pc, device, caller, bytes, alignment, manager, flags, request);
        }
        else
            PublishedCompletionProbe().AllocationBegin(thread, pc, device, caller, request, alignment, manager, flags);
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionAllocationTarget(std::uint32_t thread, std::uint32_t target, std::uint32_t context, std::uint32_t requestPointer, std::uint32_t manager) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        PublishedCompletionProbe().AllocationTarget(thread, target, context, readWord(requestPointer), { readWord(manager, 0xA4), readWord(manager, 0xA8), readWord(manager, 0xAC) });
    }
    catch (...)
    {
        failure();
    }
}

void ObserveCompletionAllocationEnd(std::uint32_t thread, std::uint32_t pc, std::uint32_t result) noexcept
{
    if (!CompletionProbeRequested())
        return;
    try
    {
        std::uint32_t base = result, size = 0;
        const auto    physical = GuestPhysicalAddress(result);
        if (physical >= kExtentBegin && physical < kExtentEnd)
        {
            auto* kernel = rex::system::kernel_state();
            auto* memory = kernel ? kernel->memory() : nullptr;
            auto* heap   = memory ? memory->LookupHeap(result) : nullptr;
            if (!heap || !heap->QueryBaseAndSize(&base, &size))
                size = 0;
        }
        PublishedCompletionProbe().AllocationEnd(thread, pc, result, physical, base, size);
    }
    catch (...)
    {
        failure();
    }
}

std::string FormatCompletionProbeCounts(const CompletionCounts& c)
{
    std::string                          fields = std::format(" cp_c810_enter={} cp_c810_exit={} cp_c810_exit_n_nonzero={} cp_init_success={} cp_init_failure={} cp_dropped={} cp_d={} cp_balance_ok={} cp_balance_bad={} cp_balance_indeterminate={} cp_p_odd={} cp_p_even={} cp_p_zero={} cp_mask_nonzero={} cp_mask_change={} cp_queue_nonzero={} cp_queue_cursor_change={} cp_queue_segment_change={} cp_s2_change={} cp_callbacks={} cp_extent_hit={} cp_extent_owner_joined={} cp_unreadable={} cp_event_drop={} cp_pair_error={} cp_epoch_ambiguous={} cp_watchdog_reset={} cp_unmatched={} cp_slot_coverage=sampled",
                                                              c.c810Enter,
                                                              c.c810Exit,
                                                              c.c810ExitNNonzero,
                                                              c.initSuccess,
                                                              c.initFailure,
                                                              c.dropped,
                                                              c.d,
                                                              c.balanceOk,
                                                              c.balanceBad,
                                                              c.balanceIndeterminate,
                                                              c.pOdd,
                                                              c.pEven,
                                                              c.pZero,
                                                              c.maskNonzero,
                                                              c.maskChange,
                                                              c.queueNonzero,
                                                              c.queueCursorChange,
                                                              c.queueSegmentChange,
                                                              c.s2Change,
                                                              c.callbacks,
                                                              c.extentHit,
                                                              c.extentOwnerJoined,
                                                              c.unreadable,
                                                              c.eventDrop,
                                                              c.pairError,
                                                              c.epochAmbiguous,
                                                              c.watchdogReset,
                                                              c.unmatched);
    constexpr std::array<const char*, 3> paths  = { "deferred", "direct", "stub" };
    for (std::size_t i = 0; i < paths.size(); ++i)
        fields += std::format(" cp_add_{}={} cp_add_{}_calls={}", paths[i], c.adds[i], paths[i], c.addCalls[i]);
    constexpr std::array<const char*, 5> slots = { "347c", "409c", "40a0", "s2_10", "s2_14" };
    for (std::size_t i = 0; i < slots.size(); ++i)
        fields += std::format(" cp_slot_{}_samples={} cp_slot_{}_nonzero={} cp_slot_{}_change={}", slots[i], c.slotSamples[i], slots[i], c.slotNonzero[i], slots[i], c.slotChange[i]);
    return fields;
}

std::string ReportCompletionProbe(bool shutdown) noexcept
{
    if (!CompletionProbeRequested())
        return {};
    try
    {
        const auto report = PublishedCompletionProbe().Take();
        // Formatting and logging occur here, never at a guest spin-lock observation.
        for (std::size_t i = 0; i < report.size; ++i)
        {
            const auto& r = report.records[i];
            std::string words;
            for (std::size_t j = 0; j < r.snapshot.words.size(); ++j)
                if (r.snapshot.Has(j))
                    words += std::format(" {}:{:08X}", j, r.snapshot.words[j]);
            std::string payload;
            for (const auto value : r.payload)
                payload += std::format(" {:08X}", value);
            const bool exact  = std::string_view(r.kind) == "add" || std::string_view(r.kind) == "retire";
            const auto writer = exact ? (std::string_view(r.kind) == "add" && r.payload[3] == 2 ? std::string("host_RaiseForDirectSubmit") : std::format("{:08X}", r.pc)) : std::string("unknown");
            REXLOG_INFO("Native completion probe: kind={} seq={} G={:08X} epoch={} thread={} pc={:08X} call={} previous_call={} add_total={} d_total={} balance={} writer_pc={} slot_coverage=sampled valid={:X} unreadable={} words=[{}] payload_valid={:X} payload=[{}]",
                        r.kind,
                        r.sequence,
                        r.device,
                        r.epoch,
                        r.thread,
                        r.pc,
                        r.call,
                        r.previousCall,
                        r.additions,
                        r.retirements,
                        static_cast<unsigned>(r.balance),
                        writer,
                        r.snapshot.valid,
                        r.snapshot.unreadable,
                        words,
                        r.payloadValid,
                        payload);
        }
        if (shutdown)
            REXLOG_INFO("Native completion probe: shutdown{}", FormatCompletionProbeCounts(report.counts));
        return FormatCompletionProbeCounts(report.counts);
    }
    catch (...)
    {
        failure();
    }
    return {};
}

} // namespace rerevved::gpu::guest_d3d
