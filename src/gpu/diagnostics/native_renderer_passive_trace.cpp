#include "gpu/diagnostics/native_renderer_passive_trace.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/thread.h>

namespace rerevved::gpu::diagnostics
{
namespace
{

constexpr std::uint64_t kSequenceMask     = 0xFFFFFFFFull;
constexpr auto          kWriterDrainLimit = std::chrono::milliseconds(100);

std::string_view pointName(PassiveTracePoint point) noexcept
{
    switch (point)
    {
        case PassiveTracePoint::Started:
            return "trace_started";
        case PassiveTracePoint::RingResetBegin:
            return "ring_reset_begin";
        case PassiveTracePoint::RingResetReturn:
            return "ring_reset_return";
        case PassiveTracePoint::ReservationEnter:
            return "reservation_enter";
        case PassiveTracePoint::ReservationReturn:
            return "reservation_return";
        case PassiveTracePoint::VdSwapOwnerEnter:
            return "vdswap_owner_enter";
        case PassiveTracePoint::VdSwapOwnerReturn:
            return "vdswap_owner_return";
        case PassiveTracePoint::VdSwapCall:
            return "vdswap_call";
        case PassiveTracePoint::VdSwapReturn:
            return "vdswap_return";
        case PassiveTracePoint::VdSwapPublished:
            return "vdswap_published";
        case PassiveTracePoint::ResolveEnter:
            return "resolve_enter";
        case PassiveTracePoint::ResolveReturn:
            return "resolve_return";
        case PassiveTracePoint::PreSwapEnter:
            return "pre_swap_enter";
        case PassiveTracePoint::PreSwapReturn:
            return "pre_swap_return";
        case PassiveTracePoint::EmitterCd20Enter:
            return "emitter_cd20_enter";
        case PassiveTracePoint::EmitterCd20Return:
            return "emitter_cd20_return";
        case PassiveTracePoint::EmitterBf40Enter:
            return "emitter_bf40_enter";
        case PassiveTracePoint::EmitterBf40Return:
            return "emitter_bf40_return";
        case PassiveTracePoint::CallbackEnter:
            return "callback_enter";
        case PassiveTracePoint::CallbackReturn:
            return "callback_return";
        case PassiveTracePoint::OrdinaryCallerEnter:
            return "ordinary_caller_enter";
        case PassiveTracePoint::OrdinaryCallerReturn:
            return "ordinary_caller_return";
        case PassiveTracePoint::AlternateCallerEnter:
            return "alternate_caller_enter";
        case PassiveTracePoint::AlternateCallerReturn:
            return "alternate_caller_return";
    }
    return "invalid";
}

void saturatingIncrement(std::atomic<std::uint32_t>& value) noexcept
{
    std::uint32_t current = value.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current,
                                        current + 1,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed))
    {
    }
}

} // namespace

PassiveTraceRecordLease::PassiveTraceRecordLease(
    PassiveTraceBuffer* buffer,
    std::uint64_t       ticket) noexcept
: buffer(buffer)
, ticket(ticket)
{
}

PassiveTraceRecordLease::~PassiveTraceRecordLease()
{
    release();
}

PassiveTraceRecordLease::PassiveTraceRecordLease(
    PassiveTraceRecordLease&& other) noexcept
: buffer(other.buffer)
, ticket(other.ticket)
{
    other.buffer = nullptr;
}

PassiveTraceRecordLease& PassiveTraceRecordLease::operator=(
    PassiveTraceRecordLease&& other) noexcept
{
    if (this != &other)
    {
        release();
        buffer       = other.buffer;
        ticket       = other.ticket;
        other.buffer = nullptr;
    }
    return *this;
}

PassiveTraceRecordLease::operator bool() const noexcept
{
    return buffer != nullptr;
}

bool PassiveTraceRecordLease::Commit(PassiveTraceEvent event) noexcept
{
    if (!buffer)
    {
        return false;
    }
    PassiveTraceBuffer* buffer = this->buffer;
    this->buffer               = nullptr;
    const bool stored          = buffer->store(event, ticket);
    buffer->leaveWriter();
    return stored;
}

void PassiveTraceRecordLease::release() noexcept
{
    if (buffer)
    {
        buffer->leaveWriter();
        buffer = nullptr;
    }
}

bool PassiveTraceBuffer::Start(const std::filesystem::path& outputPath)
{
    if (outputPath.empty() ||
        gate.load(std::memory_order_acquire) != kGateClosed ||
        started.load(std::memory_order_acquire))
    {
        return false;
    }

    std::error_code       error;
    std::filesystem::path partialPath = outputPath;
    partialPath += ".partial";
    if (std::filesystem::exists(outputPath, error) || error ||
        std::filesystem::exists(partialPath, error) || error)
    {
        return false;
    }

    this->outputPath = outputPath;
    for (auto& slot : slots)
    {
        slot.committed.store(false, std::memory_order_relaxed);
        slot.event = {};
    }
    nextSlot.store(0, std::memory_order_relaxed);
    overflow.store(0, std::memory_order_relaxed);
    inFlightAtFlush.store(0, std::memory_order_relaxed);
    epochTransitionFailures.store(0, std::memory_order_relaxed);
    epochSequence.store((std::uint64_t{ 1 } << 32) | 1,
                        std::memory_order_relaxed);
    flushed.store(false, std::memory_order_relaxed);
    started.store(true, std::memory_order_release);
    gate.store(0, std::memory_order_release);

    PassiveTraceEvent started{};
    started.point = PassiveTracePoint::Started;
    return Record(started);
}

bool PassiveTraceBuffer::StopAndFlush()
{
    if (!started.load(std::memory_order_acquire) ||
        flushed.load(std::memory_order_acquire))
    {
        return true;
    }

    std::uint32_t state =
        gate.fetch_or(kGateClosed, std::memory_order_acq_rel) | kGateClosed;
    gate.fetch_and(~kGateEpoch, std::memory_order_acq_rel);
    state = gate.load(std::memory_order_acquire);

    const auto deadline = std::chrono::steady_clock::now() +
                          kWriterDrainLimit;
    while ((state & kGateWritersMask) != 0 &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        state = gate.load(std::memory_order_acquire);
    }
    const std::uint32_t inFlight = state & kGateWritersMask;
    inFlightAtFlush.store(inFlight, std::memory_order_release);
    if (inFlight != 0)
    {
        return false;
    }

    if (!serialize(0))
    {
        return false;
    }
    flushed.store(true, std::memory_order_release);
    return true;
}

bool PassiveTraceBuffer::Enabled() const noexcept
{
    return (gate.load(std::memory_order_acquire) & kGateClosed) == 0;
}

bool PassiveTraceBuffer::enterWriter() noexcept
{
    std::uint32_t state = gate.load(std::memory_order_acquire);
    while ((state & kGateClosed) == 0)
    {
        if ((state & kGateWritersMask) == kGateWritersMask)
        {
            return false;
        }
        if (gate.compare_exchange_weak(state,
                                       state + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

void PassiveTraceBuffer::leaveWriter() noexcept
{
    gate.fetch_sub(1, std::memory_order_release);
}

std::uint64_t PassiveTraceBuffer::nextTicket() noexcept
{
    return epochSequence.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t PassiveTraceBuffer::nextEpochTicket() noexcept
{
    std::uint64_t current = epochSequence.load(std::memory_order_relaxed);
    for (;;)
    {
        const std::uint64_t epoch    = current >> 32;
        const std::uint64_t sequence = current & kSequenceMask;
        const std::uint64_t next     = ((epoch + 1) << 32) | (sequence + 1);
        if (epochSequence.compare_exchange_weak(current,
                                                next,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed))
        {
            return ((epoch + 1) << 32) | sequence;
        }
    }
}

PassiveTraceRecordLease PassiveTraceBuffer::BeginRecord() noexcept
{
    if (!enterWriter())
    {
        return {};
    }
    return PassiveTraceRecordLease(this, nextTicket());
}

bool PassiveTraceBuffer::store(PassiveTraceEvent event,
                               std::uint64_t     ticket) noexcept
{
    std::uint32_t slotIndex = nextSlot.load(std::memory_order_relaxed);
    while (slotIndex < kPassiveTraceCapacity &&
           !nextSlot.compare_exchange_weak(slotIndex,
                                           slotIndex + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
    {
    }
    if (slotIndex >= kPassiveTraceCapacity)
    {
        saturatingIncrement(overflow);
        return false;
    }

    event.sequence         = ticket & kSequenceMask;
    event.epoch            = static_cast<std::uint32_t>(ticket >> 32);
    event.threadId         = rex::thread::current_thread_id();
    slots[slotIndex].event = event;
    slots[slotIndex].committed.store(true, std::memory_order_release);
    return true;
}

bool PassiveTraceBuffer::Record(PassiveTraceEvent event) noexcept
{
    auto lease = BeginRecord();
    if (!lease)
    {
        return false;
    }
    return lease.Commit(event);
}

bool PassiveTraceBuffer::BeginObservationEpoch(
    PassiveTraceEvent event) noexcept
{
    std::uint32_t state          = gate.load(std::memory_order_acquire);
    bool          ownsTransition = false;
    while ((state & kGateClosed) == 0)
    {
        if ((state & kGateWritersMask) == kGateWritersMask)
        {
            return false;
        }
        const std::uint32_t epochState =
            state + 1 + kGateClosed + kGateEpoch;
        if (gate.compare_exchange_weak(state,
                                       epochState,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire))
        {
            state          = epochState;
            ownsTransition = true;
            break;
        }
    }
    if (!ownsTransition)
    {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          kWriterDrainLimit;
    while ((state & kGateWritersMask) != 1 &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        state = gate.load(std::memory_order_acquire);
    }
    if ((state & kGateWritersMask) != 1)
    {
        saturatingIncrement(epochTransitionFailures);
        gate.fetch_and(~kGateEpoch, std::memory_order_acq_rel);
        leaveWriter();
        return false;
    }
    if ((state & kGateEpoch) == 0)
    {
        leaveWriter();
        return false;
    }

    const bool    stored   = store(event, nextEpochTicket());
    std::uint32_t expected = kGateClosed | kGateEpoch | 1;
    if (!gate.compare_exchange_strong(expected,
                                      0,
                                      std::memory_order_release,
                                      std::memory_order_acquire))
    {
        leaveWriter();
        return false;
    }
    return stored;
}

PassiveTraceStatistics PassiveTraceBuffer::Statistics() const noexcept
{
    const std::uint64_t epochSequenceValue =
        epochSequence.load(std::memory_order_acquire);
    const std::uint32_t slots = nextSlot.load(std::memory_order_acquire);
    return {
        std::min<std::uint32_t>(slots,
                                static_cast<std::uint32_t>(kPassiveTraceCapacity)),
        overflow.load(std::memory_order_acquire),
        inFlightAtFlush.load(std::memory_order_acquire),
        epochTransitionFailures.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(epochSequenceValue >> 32),
        (epochSequenceValue & kSequenceMask) == 0
            ? 0
            : (epochSequenceValue & kSequenceMask) - 1,
    };
}

bool PassiveTraceBuffer::serialize(std::uint32_t inFlight)
{
    std::error_code error;
    if (const auto parent = outputPath.parent_path(); !parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return false;
        }
    }

    std::filesystem::path partialPath = outputPath;
    partialPath += ".partial";
    std::ofstream output(partialPath, std::ios::out | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    std::vector<PassiveTraceEvent> records;
    records.reserve(kPassiveTraceCapacity);
    for (const auto& slot : slots)
    {
        if (slot.committed.load(std::memory_order_acquire))
        {
            records.push_back(slot.event);
        }
    }
    std::sort(records.begin(),
              records.end(),
              [](const PassiveTraceEvent& left,
                 const PassiveTraceEvent& right)
              {
                  return left.sequence < right.sequence;
              });

    output << "# schema=native_resolve_vdswap_passive_trace_v1\n";
    output << "# capacity=" << kPassiveTraceCapacity << '\n';
    output << "# stored=" << records.size() << '\n';
    output << "# overflow=" << overflow.load(std::memory_order_acquire) << '\n';
    output << "# in_flight_at_flush=" << inFlight << '\n';
    output << "# epoch_transition_failures="
           << epochTransitionFailures.load(std::memory_order_acquire) << '\n';
    output << "sequence,thread_id,epoch,event,valid_fields,device_address,"
              "requested_dwords,reservation_address,vdswap_argument,return_value,"
              "resolve_resource_address,descriptor_address,resolve_call_address,"
              "resolve_flags,resolve_mip_level,resolve_slice,device_position,"
              "device_end,system_state_2a94,published_write_pointer,"
              "read_pointer_writeback_address,read_pointer_writeback";
    for (std::size_t index = 0; index < kPassiveTraceDescriptorDwords; ++index)
    {
        output << ",descriptor_" << std::setw(2) << std::setfill('0') << index;
    }
    for (std::size_t index = 0; index < kPassiveTraceReservationDwords; ++index)
    {
        output << ",reservation_" << std::setw(2) << std::setfill('0') << index;
    }
    output << '\n'
           << std::hex << std::uppercase << std::setfill('0');

    for (const auto& event : records)
    {
        output << std::dec << event.sequence << ',' << event.threadId << ','
               << event.epoch << ',' << pointName(event.point) << std::hex
               << ',' << event.validFields << ',' << event.deviceAddress
               << ',' << event.requestedDwords << ',' << event.reservationAddress
               << ',' << event.vdswapArgument << ',' << event.returnValue
               << ',' << event.resolveResourceAddress << ','
               << event.descriptorAddress << ',' << event.resolveCallAddress
               << ',' << event.resolveFlags << ',' << event.resolveMipLevel
               << ',' << event.resolveSlice << ',' << event.devicePosition
               << ',' << event.deviceEnd << ',' << event.systemState2a94
               << ',' << event.publishedWritePointer << ','
               << event.readPointerWritebackAddress << ','
               << event.readPointerWriteback;
        for (const std::uint32_t word : event.descriptor)
        {
            output << ',' << word;
        }
        for (const std::uint32_t word : event.reservationWords)
        {
            output << ',' << word;
        }
        output << '\n';
    }
    const bool written = output.good();
    output.close();
    if (!written || std::filesystem::exists(outputPath, error) || error)
    {
        return false;
    }
    std::filesystem::rename(partialPath, outputPath, error);
    return !error;
}

PassiveTraceBuffer& GetPassiveTraceBuffer() noexcept
{
    static PassiveTraceBuffer buffer;
    return buffer;
}

} // namespace rerevved::gpu::diagnostics
