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

std::string_view PointName(PassiveTracePoint point) noexcept
{
    switch (point)
    {
        case PassiveTracePoint::kTraceStarted:
            return "trace_started";
        case PassiveTracePoint::kRingResetBegin:
            return "ring_reset_begin";
        case PassiveTracePoint::kRingResetReturn:
            return "ring_reset_return";
        case PassiveTracePoint::kReservationEnter:
            return "reservation_enter";
        case PassiveTracePoint::kReservationReturn:
            return "reservation_return";
        case PassiveTracePoint::kVdSwapOwnerEnter:
            return "vdswap_owner_enter";
        case PassiveTracePoint::kVdSwapOwnerReturn:
            return "vdswap_owner_return";
        case PassiveTracePoint::kVdSwapCall:
            return "vdswap_call";
        case PassiveTracePoint::kVdSwapReturn:
            return "vdswap_return";
        case PassiveTracePoint::kVdSwapPublished:
            return "vdswap_published";
        case PassiveTracePoint::kResolveEnter:
            return "resolve_enter";
        case PassiveTracePoint::kResolveReturn:
            return "resolve_return";
        case PassiveTracePoint::kPreSwapEnter:
            return "pre_swap_enter";
        case PassiveTracePoint::kPreSwapReturn:
            return "pre_swap_return";
        case PassiveTracePoint::kEmitterCd20Enter:
            return "emitter_cd20_enter";
        case PassiveTracePoint::kEmitterCd20Return:
            return "emitter_cd20_return";
        case PassiveTracePoint::kEmitterBf40Enter:
            return "emitter_bf40_enter";
        case PassiveTracePoint::kEmitterBf40Return:
            return "emitter_bf40_return";
        case PassiveTracePoint::kCallbackEnter:
            return "callback_enter";
        case PassiveTracePoint::kCallbackReturn:
            return "callback_return";
        case PassiveTracePoint::kOrdinaryCallerEnter:
            return "ordinary_caller_enter";
        case PassiveTracePoint::kOrdinaryCallerReturn:
            return "ordinary_caller_return";
        case PassiveTracePoint::kAlternateCallerEnter:
            return "alternate_caller_enter";
        case PassiveTracePoint::kAlternateCallerReturn:
            return "alternate_caller_return";
    }
    return "invalid";
}

void SaturatingIncrement(std::atomic<std::uint32_t>& value) noexcept
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
: buffer_(buffer)
, ticket_(ticket)
{
}

PassiveTraceRecordLease::~PassiveTraceRecordLease()
{
    Release();
}

PassiveTraceRecordLease::PassiveTraceRecordLease(
    PassiveTraceRecordLease&& other) noexcept
: buffer_(other.buffer_)
, ticket_(other.ticket_)
{
    other.buffer_ = nullptr;
}

PassiveTraceRecordLease& PassiveTraceRecordLease::operator=(
    PassiveTraceRecordLease&& other) noexcept
{
    if (this != &other)
    {
        Release();
        buffer_       = other.buffer_;
        ticket_       = other.ticket_;
        other.buffer_ = nullptr;
    }
    return *this;
}

PassiveTraceRecordLease::operator bool() const noexcept
{
    return buffer_ != nullptr;
}

bool PassiveTraceRecordLease::Commit(PassiveTraceEvent event) noexcept
{
    if (!buffer_)
    {
        return false;
    }
    PassiveTraceBuffer* buffer = buffer_;
    buffer_                    = nullptr;
    const bool stored          = buffer->Store(event, ticket_);
    buffer->LeaveWriter();
    return stored;
}

void PassiveTraceRecordLease::Release() noexcept
{
    if (buffer_)
    {
        buffer_->LeaveWriter();
        buffer_ = nullptr;
    }
}

bool PassiveTraceBuffer::Start(const std::filesystem::path& output_path)
{
    if (output_path.empty() ||
        gate_.load(std::memory_order_acquire) != kGateClosed ||
        started_.load(std::memory_order_acquire))
    {
        return false;
    }

    std::error_code       error;
    std::filesystem::path partial_path = output_path;
    partial_path += ".partial";
    if (std::filesystem::exists(output_path, error) || error ||
        std::filesystem::exists(partial_path, error) || error)
    {
        return false;
    }

    output_path_ = output_path;
    for (auto& slot : slots_)
    {
        slot.committed.store(false, std::memory_order_relaxed);
        slot.event = {};
    }
    next_slot_.store(0, std::memory_order_relaxed);
    overflow_.store(0, std::memory_order_relaxed);
    in_flight_at_flush_.store(0, std::memory_order_relaxed);
    epoch_transition_failures_.store(0, std::memory_order_relaxed);
    epoch_sequence_.store((std::uint64_t{ 1 } << 32) | 1,
                          std::memory_order_relaxed);
    flushed_.store(false, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    gate_.store(0, std::memory_order_release);

    PassiveTraceEvent started{};
    started.point = PassiveTracePoint::kTraceStarted;
    return Record(started);
}

bool PassiveTraceBuffer::StopAndFlush()
{
    if (!started_.load(std::memory_order_acquire) ||
        flushed_.load(std::memory_order_acquire))
    {
        return true;
    }

    std::uint32_t state =
        gate_.fetch_or(kGateClosed, std::memory_order_acq_rel) | kGateClosed;
    gate_.fetch_and(~kGateEpoch, std::memory_order_acq_rel);
    state = gate_.load(std::memory_order_acquire);

    const auto deadline = std::chrono::steady_clock::now() +
                          kWriterDrainLimit;
    while ((state & kGateWritersMask) != 0 &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        state = gate_.load(std::memory_order_acquire);
    }
    const std::uint32_t in_flight = state & kGateWritersMask;
    in_flight_at_flush_.store(in_flight, std::memory_order_release);
    if (in_flight != 0)
    {
        return false;
    }

    if (!Serialize(0))
    {
        return false;
    }
    flushed_.store(true, std::memory_order_release);
    return true;
}

bool PassiveTraceBuffer::enabled() const noexcept
{
    return (gate_.load(std::memory_order_acquire) & kGateClosed) == 0;
}

bool PassiveTraceBuffer::EnterWriter() noexcept
{
    std::uint32_t state = gate_.load(std::memory_order_acquire);
    while ((state & kGateClosed) == 0)
    {
        if ((state & kGateWritersMask) == kGateWritersMask)
        {
            return false;
        }
        if (gate_.compare_exchange_weak(state,
                                        state + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

void PassiveTraceBuffer::LeaveWriter() noexcept
{
    gate_.fetch_sub(1, std::memory_order_release);
}

std::uint64_t PassiveTraceBuffer::NextTicket() noexcept
{
    return epoch_sequence_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t PassiveTraceBuffer::NextEpochTicket() noexcept
{
    std::uint64_t current = epoch_sequence_.load(std::memory_order_relaxed);
    for (;;)
    {
        const std::uint64_t epoch    = current >> 32;
        const std::uint64_t sequence = current & kSequenceMask;
        const std::uint64_t next     = ((epoch + 1) << 32) | (sequence + 1);
        if (epoch_sequence_.compare_exchange_weak(current,
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
    if (!EnterWriter())
    {
        return {};
    }
    return PassiveTraceRecordLease(this, NextTicket());
}

bool PassiveTraceBuffer::Store(PassiveTraceEvent event,
                               std::uint64_t     ticket) noexcept
{
    std::uint32_t slot_index = next_slot_.load(std::memory_order_relaxed);
    while (slot_index < kPassiveTraceCapacity &&
           !next_slot_.compare_exchange_weak(slot_index,
                                             slot_index + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
    {
    }
    if (slot_index >= kPassiveTraceCapacity)
    {
        SaturatingIncrement(overflow_);
        return false;
    }

    event.sequence           = ticket & kSequenceMask;
    event.epoch              = static_cast<std::uint32_t>(ticket >> 32);
    event.thread_id          = rex::thread::current_thread_id();
    slots_[slot_index].event = event;
    slots_[slot_index].committed.store(true, std::memory_order_release);
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
    std::uint32_t state           = gate_.load(std::memory_order_acquire);
    bool          owns_transition = false;
    while ((state & kGateClosed) == 0)
    {
        if ((state & kGateWritersMask) == kGateWritersMask)
        {
            return false;
        }
        const std::uint32_t epoch_state =
            state + 1 + kGateClosed + kGateEpoch;
        if (gate_.compare_exchange_weak(state,
                                        epoch_state,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        {
            state           = epoch_state;
            owns_transition = true;
            break;
        }
    }
    if (!owns_transition)
    {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          kWriterDrainLimit;
    while ((state & kGateWritersMask) != 1 &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        state = gate_.load(std::memory_order_acquire);
    }
    if ((state & kGateWritersMask) != 1)
    {
        SaturatingIncrement(epoch_transition_failures_);
        gate_.fetch_and(~kGateEpoch, std::memory_order_acq_rel);
        LeaveWriter();
        return false;
    }
    if ((state & kGateEpoch) == 0)
    {
        LeaveWriter();
        return false;
    }

    const bool    stored   = Store(event, NextEpochTicket());
    std::uint32_t expected = kGateClosed | kGateEpoch | 1;
    if (!gate_.compare_exchange_strong(expected,
                                       0,
                                       std::memory_order_release,
                                       std::memory_order_acquire))
    {
        LeaveWriter();
        return false;
    }
    return stored;
}

PassiveTraceStatistics PassiveTraceBuffer::statistics() const noexcept
{
    const std::uint64_t epoch_sequence =
        epoch_sequence_.load(std::memory_order_acquire);
    const std::uint32_t slots = next_slot_.load(std::memory_order_acquire);
    return {
        std::min<std::uint32_t>(slots,
                                static_cast<std::uint32_t>(kPassiveTraceCapacity)),
        overflow_.load(std::memory_order_acquire),
        in_flight_at_flush_.load(std::memory_order_acquire),
        epoch_transition_failures_.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(epoch_sequence >> 32),
        (epoch_sequence & kSequenceMask) == 0
            ? 0
            : (epoch_sequence & kSequenceMask) - 1,
    };
}

bool PassiveTraceBuffer::Serialize(std::uint32_t in_flight)
{
    std::error_code error;
    if (const auto parent = output_path_.parent_path(); !parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return false;
        }
    }

    std::filesystem::path partial_path = output_path_;
    partial_path += ".partial";
    std::ofstream output(partial_path, std::ios::out | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    std::vector<PassiveTraceEvent> records;
    records.reserve(kPassiveTraceCapacity);
    for (const auto& slot : slots_)
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
    output << "# overflow=" << overflow_.load(std::memory_order_acquire) << '\n';
    output << "# in_flight_at_flush=" << in_flight << '\n';
    output << "# epoch_transition_failures="
           << epoch_transition_failures_.load(std::memory_order_acquire) << '\n';
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
        output << std::dec << event.sequence << ',' << event.thread_id << ','
               << event.epoch << ',' << PointName(event.point) << std::hex
               << ',' << event.valid_fields << ',' << event.device_address
               << ',' << event.requested_dwords << ',' << event.reservation_address
               << ',' << event.vdswap_argument << ',' << event.return_value
               << ',' << event.resolve_resource_address << ','
               << event.descriptor_address << ',' << event.resolve_call_address
               << ',' << event.resolve_flags << ',' << event.resolve_mip_level
               << ',' << event.resolve_slice << ',' << event.device_position
               << ',' << event.device_end << ',' << event.system_state_2a94
               << ',' << event.published_write_pointer << ','
               << event.read_pointer_writeback_address << ','
               << event.read_pointer_writeback;
        for (const std::uint32_t word : event.descriptor)
        {
            output << ',' << word;
        }
        for (const std::uint32_t word : event.reservation_words)
        {
            output << ',' << word;
        }
        output << '\n';
    }
    const bool written = output.good();
    output.close();
    if (!written || std::filesystem::exists(output_path_, error) || error)
    {
        return false;
    }
    std::filesystem::rename(partial_path, output_path_, error);
    return !error;
}

PassiveTraceBuffer& GetPassiveTraceBuffer() noexcept
{
    static PassiveTraceBuffer buffer;
    return buffer;
}

} // namespace rerevved::gpu::diagnostics
