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
    Started,
    RingResetBegin,
    RingResetReturn,
    ReservationEnter,
    ReservationReturn,
    VdSwapOwnerEnter,
    VdSwapOwnerReturn,
    VdSwapCall,
    VdSwapReturn,
    VdSwapPublished,
    ResolveEnter,
    ResolveReturn,
    PreSwapEnter,
    PreSwapReturn,
    EmitterCd20Enter,
    EmitterCd20Return,
    EmitterBf40Enter,
    EmitterBf40Return,
    CallbackEnter,
    CallbackReturn,
    OrdinaryCallerEnter,
    OrdinaryCallerReturn,
    AlternateCallerEnter,
    AlternateCallerReturn,
};

enum PassiveTraceValidField : std::uint32_t
{
    TRACE_DEVICE_POSITION        = 1u << 0,
    TRACE_DEVICE_END             = 1u << 1,
    TRACE_SYSTEM_STATE           = 1u << 2,
    TRACE_PUBLISHED_WRITE        = 1u << 3,
    TRACE_READ_POINTER_WRITEBACK = 1u << 4,
    TRACE_DESCRIPTOR             = 1u << 5,
    TRACE_RESERVATION_WORDS      = 1u << 6,
};

// Every address and value in this record is an observation. It does not prove
// guest ownership, publication, completion, or acknowledgement semantics.
struct PassiveTraceEvent
{
    std::uint64_t     sequence = 0;
    std::uint32_t     threadId = 0;
    std::uint32_t     epoch    = 0;
    PassiveTracePoint point    = PassiveTracePoint::Started;

    std::uint32_t validFields            = 0;
    std::uint32_t deviceAddress          = 0;
    std::uint32_t requestedDwords        = 0;
    std::uint32_t reservationAddress     = 0;
    std::uint32_t vdswapArgument         = 0;
    std::uint32_t returnValue            = 0;
    std::uint32_t resolveResourceAddress = 0;
    std::uint32_t descriptorAddress      = 0;
    std::uint32_t resolveCallAddress     = 0;
    std::uint32_t resolveFlags           = 0;
    std::uint32_t resolveMipLevel        = 0;
    std::uint32_t resolveSlice           = 0;

    std::uint32_t devicePosition              = 0;
    std::uint32_t deviceEnd                   = 0;
    std::uint32_t systemState2a94             = 0;
    std::uint32_t publishedWritePointer       = 0;
    std::uint32_t readPointerWritebackAddress = 0;
    std::uint32_t readPointerWriteback        = 0;

    std::array<std::uint32_t, kPassiveTraceDescriptorDwords>  descriptor{};
    std::array<std::uint32_t, kPassiveTraceReservationDwords> reservationWords{};
};

struct PassiveTraceStatistics
{
    std::uint32_t stored                  = 0;
    std::uint32_t overflow                = 0;
    std::uint32_t inFlightAtFlush         = 0;
    std::uint32_t epochTransitionFailures = 0;
    std::uint32_t epoch                   = 0;
    std::uint64_t lastSequence            = 0;
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
    void release() noexcept;

    PassiveTraceBuffer* buffer = nullptr;
    std::uint64_t       ticket = 0;
};

class PassiveTraceBuffer final
{
public:
    PassiveTraceBuffer() = default;

    bool Start(const std::filesystem::path& outputPath);
    bool StopAndFlush();
    bool Enabled() const noexcept;

    PassiveTraceRecordLease BeginRecord() noexcept;
    bool                    Record(PassiveTraceEvent event) noexcept;
    bool                    BeginObservationEpoch(PassiveTraceEvent event) noexcept;

    PassiveTraceStatistics Statistics() const noexcept;

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

    bool          enterWriter() noexcept;
    void          leaveWriter() noexcept;
    bool          store(PassiveTraceEvent event, std::uint64_t ticket) noexcept;
    std::uint64_t nextTicket() noexcept;
    std::uint64_t nextEpochTicket() noexcept;
    bool          serialize(std::uint32_t inFlight);

    std::array<Slot, kPassiveTraceCapacity> slots{};
    std::atomic<std::uint32_t>              gate{ kGateClosed };
    std::atomic<std::uint32_t>              nextSlot{ 0 };
    std::atomic<std::uint32_t>              overflow{ 0 };
    std::atomic<std::uint32_t>              inFlightAtFlush{ 0 };
    std::atomic<std::uint32_t>              epochTransitionFailures{ 0 };
    std::atomic<std::uint64_t>              epochSequence{ 0 };
    std::atomic<bool>                       started{ false };
    std::atomic<bool>                       flushed{ false };
    std::filesystem::path                   outputPath;
};

PassiveTraceBuffer& GetPassiveTraceBuffer() noexcept;

} // namespace rerevved::gpu::diagnostics
