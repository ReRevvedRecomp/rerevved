#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace rerevved::gpu::diagnostics
{

constexpr std::size_t kPassiveTraceCapacity          = 4096;
constexpr std::size_t kPassiveTraceDescriptorDwords  = 6;
constexpr std::size_t kPassiveTraceReservationDwords = 64;

enum class PassiveTracePoint : std::uint8_t
{
    kTraceStarted,
    kRingResetBegin,
    kRingResetReturn,
    kReservationEnter,
    kReservationReturn,
    kVdSwapOwnerEnter,
    kVdSwapOwnerReturn,
    kVdSwapCall,
    kVdSwapReturn,
    kVdSwapPublished,
    kResolveEnter,
    kResolveReturn,
    kPreSwapEnter,
    kPreSwapReturn,
    kEmitterCd20Enter,
    kEmitterCd20Return,
    kEmitterBf40Enter,
    kEmitterBf40Return,
    kCallbackEnter,
    kCallbackReturn,
    kOrdinaryCallerEnter,
    kOrdinaryCallerReturn,
    kAlternateCallerEnter,
    kAlternateCallerReturn,
};

enum PassiveTraceValidField : std::uint32_t
{
    kTraceDevicePosition       = 1u << 0,
    kTraceDeviceEnd            = 1u << 1,
    kTraceSystemState          = 1u << 2,
    kTracePublishedWrite       = 1u << 3,
    kTraceReadPointerWriteback = 1u << 4,
    kTraceDescriptor           = 1u << 5,
    kTraceReservationWords     = 1u << 6,
};

// Every address and value in this record is an observation. It does not prove
// guest ownership, publication, completion, or acknowledgement semantics.
struct PassiveTraceEvent
{
    std::uint64_t     sequence  = 0;
    std::uint32_t     thread_id = 0;
    std::uint32_t     epoch     = 0;
    PassiveTracePoint point     = PassiveTracePoint::kTraceStarted;

    std::uint32_t valid_fields             = 0;
    std::uint32_t device_address           = 0;
    std::uint32_t requested_dwords         = 0;
    std::uint32_t reservation_address      = 0;
    std::uint32_t vdswap_argument          = 0;
    std::uint32_t return_value             = 0;
    std::uint32_t resolve_resource_address = 0;
    std::uint32_t descriptor_address       = 0;
    std::uint32_t resolve_call_address     = 0;
    std::uint32_t resolve_flags            = 0;
    std::uint32_t resolve_mip_level        = 0;
    std::uint32_t resolve_slice            = 0;

    std::uint32_t device_position                = 0;
    std::uint32_t device_end                     = 0;
    std::uint32_t system_state_2a94              = 0;
    std::uint32_t published_write_pointer        = 0;
    std::uint32_t read_pointer_writeback_address = 0;
    std::uint32_t read_pointer_writeback         = 0;

    std::array<std::uint32_t, kPassiveTraceDescriptorDwords>  descriptor{};
    std::array<std::uint32_t, kPassiveTraceReservationDwords> reservation_words{};
};

struct PassiveTraceStatistics
{
    std::uint32_t stored                    = 0;
    std::uint32_t overflow                  = 0;
    std::uint32_t in_flight_at_flush        = 0;
    std::uint32_t epoch_transition_failures = 0;
    std::uint32_t epoch                     = 0;
    std::uint64_t last_sequence             = 0;
};

class PassiveTraceBuffer;

class PassiveTraceRecordLease final
{
public:
    PassiveTraceRecordLease() = default;
    ~PassiveTraceRecordLease();

    PassiveTraceRecordLease(const PassiveTraceRecordLease&)            = delete;
    PassiveTraceRecordLease& operator=(const PassiveTraceRecordLease&) = delete;

    PassiveTraceRecordLease(PassiveTraceRecordLease&& other) noexcept;
    PassiveTraceRecordLease& operator=(PassiveTraceRecordLease&& other) noexcept;

    explicit operator bool() const noexcept;
    bool     Commit(PassiveTraceEvent event) noexcept;

private:
    friend class PassiveTraceBuffer;

    PassiveTraceRecordLease(PassiveTraceBuffer* buffer,
                            std::uint64_t       ticket) noexcept;
    void Release() noexcept;

    PassiveTraceBuffer* buffer_ = nullptr;
    std::uint64_t       ticket_ = 0;
};

class PassiveTraceBuffer final
{
public:
    PassiveTraceBuffer() = default;

    bool Start(const std::filesystem::path& output_path);
    bool StopAndFlush();
    bool enabled() const noexcept;

    PassiveTraceRecordLease BeginRecord() noexcept;
    bool                    Record(PassiveTraceEvent event) noexcept;
    bool                    BeginObservationEpoch(PassiveTraceEvent event) noexcept;

    PassiveTraceStatistics statistics() const noexcept;

private:
    friend class PassiveTraceRecordLease;

    struct Slot
    {
        std::atomic<bool> committed{ false };
        PassiveTraceEvent event{};
    };

    static constexpr std::uint32_t kGateClosed      = 0x80000000u;
    static constexpr std::uint32_t kGateEpoch       = 0x40000000u;
    static constexpr std::uint32_t kGateWritersMask = ~(kGateClosed | kGateEpoch);

    bool          EnterWriter() noexcept;
    void          LeaveWriter() noexcept;
    bool          Store(PassiveTraceEvent event, std::uint64_t ticket) noexcept;
    std::uint64_t NextTicket() noexcept;
    std::uint64_t NextEpochTicket() noexcept;
    bool          Serialize(std::uint32_t in_flight);

    std::array<Slot, kPassiveTraceCapacity> slots_{};
    std::atomic<std::uint32_t>              gate_{ kGateClosed };
    std::atomic<std::uint32_t>              next_slot_{ 0 };
    std::atomic<std::uint32_t>              overflow_{ 0 };
    std::atomic<std::uint32_t>              in_flight_at_flush_{ 0 };
    std::atomic<std::uint32_t>              epoch_transition_failures_{ 0 };
    std::atomic<std::uint64_t>              epoch_sequence_{ 0 };
    std::atomic<bool>                       started_{ false };
    std::atomic<bool>                       flushed_{ false };
    std::filesystem::path                   output_path_;
};

PassiveTraceBuffer& GetPassiveTraceBuffer() noexcept;

} // namespace rerevved::gpu::diagnostics
