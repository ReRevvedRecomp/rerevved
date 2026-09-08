#ifndef REREVVED_NATIVE_RENDERER_COVERAGE_H
#define REREVVED_NATIVE_RENDERER_COVERAGE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rerevved::native_renderer
{

inline constexpr std::size_t   kSegmentCount       = 8;
inline constexpr std::size_t   kOperationCount     = 1;
inline constexpr std::size_t   kValueDomainCount   = 2;
inline constexpr std::size_t   kHookSiteCount      = 2;
inline constexpr std::size_t   kCheckpointCapacity = 6;
inline constexpr std::uint32_t kMaxInFlight        = 64;
// Generated metadata publishes the measured fixed-object budget. Keep this
// value in the public contract without including the generated implementation
// file in every consumer.
inline constexpr std::size_t kObserverByteBudget = 3456;

// Generated metadata is included by the implementation unit only; this public
// header remains independent of SDK and generator headers.
void RecordSiteFixedValue(std::uint32_t site_index, std::int64_t value) noexcept;

enum class StartStatus : std::uint8_t
{
    Accepted = 0,
    AlreadyStarted,
    AlreadyFinalized,
    InvalidRunId,
    InvalidTransitionId,
    InvalidInputDigest,
    InvalidXenos,
    InvalidRov,
    InvalidOutputDirectory,
    InvalidOutputRoot,
};

enum class ExitClass : std::uint8_t
{
    GuestComplete = 0,
    WindowClose,
    Shutdown,
};

enum class FinalizeStatus : std::uint8_t
{
    Accepted = 0,
    AlreadyFinalized,
    NotStarted,
    Incomplete,
    WriteFailure,
    Busy,
};

enum class CheckpointStatus : std::uint8_t
{
    Accepted = 0,
    AlreadyRecorded,
    InvalidSegment,
    InvalidMark,
    NotStarted,
    Disabled,
};

enum class AnomalyId : std::uint32_t
{
    InvalidSegment           = 9,
    InvalidCheckpointMark    = 10,
    CounterSaturated         = 11,
    FinalizationDrainTimeout = 14,
    CheckpointSequence       = 15,
};

struct StartOptions
{
    const char* run_id           = nullptr;
    const char* transition_id    = nullptr;
    const char* input_digest     = nullptr;
    const char* output_directory = nullptr;
    const char* output_root      = nullptr;
    bool        xenos_enabled    = false;
    bool        rov_enabled      = false;
};

// These fields deliberately contain only caller-owned conservative gameplay
// snapshots. A checkpoint never reads renderer state.
struct SnapshotFields
{
    std::uint64_t frame_sequence    = 0;
    std::uint32_t valid_fields      = 0;
    bool          gameplay_active   = false;
    bool          interface_update  = false;
    std::int32_t  active_player     = -1;
    std::uint32_t human_player_mask = 0;
    bool          turn_owner_known  = false;
    bool          human_turn        = false;
    bool          available         = false;
    std::int32_t  civilization      = 0;
    std::int32_t  era               = 0;
    std::int32_t  year              = 0;
    std::int32_t  turn              = 0;
};

struct CounterSnapshot
{
    std::uint64_t values[kSegmentCount][kOperationCount][kValueDomainCount][kHookSiteCount]{};
    std::uint64_t saturated_failures = 0;
    std::uint64_t rejected_in_flight = 0;
};

struct SegmentSnapshot
{
    bool           accepted = false;
    SnapshotFields fields{};
};

struct CheckpointSnapshot
{
    bool           accepted = false;
    std::uint32_t  segment  = 0;
    SnapshotFields fields{};
};

struct AnomalySnapshot
{
    std::uint32_t id    = 0;
    std::uint64_t count = 0;
};

struct ObserverSnapshot
{
    bool               started    = false;
    bool               enabled    = false;
    bool               finalized  = false;
    bool               incomplete = false;
    ExitClass          exit_class = ExitClass::GuestComplete;
    CounterSnapshot    counters{};
    SegmentSnapshot    segments[kSegmentCount]{};
    CheckpointSnapshot checkpoints[kCheckpointCapacity]{};
    AnomalySnapshot    anomalies[static_cast<std::size_t>(AnomalyId::CheckpointSequence) + 1]{};
    bool               transition_attribution_valid = true;
};

class Observer final
{
public:
    Observer() noexcept;
    static Observer& Instance() noexcept;

    StartStatus    Start(const StartOptions& options) noexcept;
    FinalizeStatus Finalize(ExitClass exit_class = ExitClass::GuestComplete) noexcept;

    CheckpointStatus RecordSegment(std::uint32_t         segment_index,
                                   const SnapshotFields& fields) noexcept;
    CheckpointStatus RecordCheckpoint(std::uint32_t         segment_index,
                                      std::uint32_t         mark_index,
                                      const SnapshotFields& fields) noexcept;

    void Snapshot(ObserverSnapshot& result) const noexcept;

#ifdef REREVVED_NATIVE_RENDERER_COVERAGE_TESTING
    bool SetCounterForTest(std::uint32_t segment,
                           std::uint32_t operation,
                           std::uint32_t domain,
                           std::uint32_t site,
                           std::uint64_t value) noexcept;
    void SetInFlightForTest(std::uint32_t value) noexcept;
    void RecordSiteFixedValueForTest(std::uint32_t site_index,
                                     std::int64_t  value) noexcept;

    std::size_t ObjectSizeBytes() const noexcept
    {
        return sizeof(*this);
    }
#endif

private:
    friend void RecordSiteFixedValue(std::uint32_t, std::int64_t) noexcept;

    struct CheckpointSlot
    {
        std::atomic<bool>          accepted{ false };
        std::atomic<std::uint32_t> segment{ 0 };
        std::atomic<std::uint64_t> frame_sequence{ 0 };
        std::atomic<std::uint32_t> valid_fields{ 0 };
        std::atomic<bool>          gameplay_active{ false };
        std::atomic<bool>          interface_update{ false };
        std::atomic<std::int32_t>  active_player{ -1 };
        std::atomic<std::uint32_t> human_player_mask{ 0 };
        std::atomic<bool>          turn_owner_known{ false };
        std::atomic<bool>          human_turn{ false };
        std::atomic<bool>          available{ false };
        std::atomic<std::int32_t>  civilization{ 0 };
        std::atomic<std::int32_t>  era{ 0 };
        std::atomic<std::int32_t>  year{ 0 };
        std::atomic<std::int32_t>  turn{ 0 };
    };

    enum class IncrementResult : std::uint8_t
    {
        Incremented = 0,
        Saturated,
        Busy,
    };

    Observer(const Observer&)            = delete;
    Observer& operator=(const Observer&) = delete;

    static Observer storage_;

    void                       ResetForStart() noexcept;
    void                       RecordSiteHot(std::uint32_t site_index, std::int64_t value) noexcept;
    IncrementResult            IncrementSaturating(std::atomic<std::uint64_t>& counter) noexcept;
    void                       RecordIncrementResult(IncrementResult result) noexcept;
    void                       IncrementAggregate(std::atomic<std::uint64_t>& counter) noexcept;
    bool                       BeginAdmission() noexcept;
    void                       EndAdmission() noexcept;
    void                       RecordAnomaly(AnomalyId id) noexcept;
    static void                StoreSnapshot(CheckpointSlot& slot, std::uint32_t segment_index, const SnapshotFields& fields) noexcept;
    static void                LoadSnapshot(const CheckpointSlot& slot,
                                            SnapshotFields&       fields) noexcept;
    std::atomic<bool>          started_{ false };
    std::atomic<bool>          enabled_{ false };
    std::atomic<bool>          finalized_{ false };
    std::atomic<bool>          finalizing_{ false };
    std::atomic<bool>          incomplete_{ false };
    std::atomic<std::uint32_t> active_segment_{ 0 };
    std::atomic<std::uint32_t> next_checkpoint_mark_{ 0 };
    std::atomic<std::uint32_t> in_flight_{ 0 };

    std::atomic<std::uint64_t>
                               counters_[kSegmentCount][kOperationCount][kValueDomainCount][kHookSiteCount]{};
    std::atomic<std::uint64_t> anomaly_counts_[static_cast<std::size_t>(AnomalyId::CheckpointSequence) + 1]{};

    CheckpointSlot checkpoints_[kCheckpointCapacity]{};
    CheckpointSlot segments_[kSegmentCount]{};

    std::atomic<std::uint64_t> saturated_failures_{ 0 };
    std::atomic<std::uint64_t> rejected_in_flight_{ 0 };
    std::atomic<bool>          transition_attribution_valid_{ true };

    ExitClass exit_class_           = ExitClass::GuestComplete;
    bool      recovered_incomplete_ = false;
    char      run_id_[32]{};
    char      transition_id_[32]{};
    char      input_digest_[129]{};
    char      output_directory_[1024]{};
    char      output_root_[1024]{};
};

// Process-wide wrappers keep generated code independent from observer object
// ownership and preserve the required zero-allocation call boundary.
StartStatus      Start(const StartOptions& options) noexcept;
FinalizeStatus   Finalize(ExitClass exit_class = ExitClass::GuestComplete) noexcept;
CheckpointStatus RecordCheckpoint(std::uint32_t         segment_index,
                                  std::uint32_t         mark_index,
                                  const SnapshotFields& fields) noexcept;
CheckpointStatus RecordSegment(std::uint32_t         segment_index,
                               const SnapshotFields& fields) noexcept;
void             Snapshot(ObserverSnapshot& result) noexcept;

} // namespace rerevved::native_renderer

#endif
