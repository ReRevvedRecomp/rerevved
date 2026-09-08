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
void RecordSiteFixedValue(std::uint32_t siteIndex, std::int64_t value) noexcept;

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
    const char* runId           = nullptr;
    const char* transitionId    = nullptr;
    const char* inputDigest     = nullptr;
    const char* outputDirectory = nullptr;
    const char* outputRoot      = nullptr;
    bool        xenosEnabled    = false;
    bool        rovEnabled      = false;
};

// These fields deliberately contain only caller-owned conservative gameplay
// snapshots. A checkpoint never reads renderer state.
struct SnapshotFields
{
    std::uint64_t frameSequence   = 0;
    std::uint32_t validFields     = 0;
    bool          gameplayActive  = false;
    bool          interfaceUpdate = false;
    std::int32_t  activePlayer    = -1;
    std::uint32_t humanPlayerMask = 0;
    bool          turnOwnerKnown  = false;
    bool          humanTurn       = false;
    bool          available       = false;
    std::int32_t  civilization    = 0;
    std::int32_t  era             = 0;
    std::int32_t  year            = 0;
    std::int32_t  turn            = 0;
};

struct CounterSnapshot
{
    std::uint64_t values[kSegmentCount][kOperationCount][kValueDomainCount][kHookSiteCount]{};
    std::uint64_t saturatedFailures = 0;
    std::uint64_t rejectedInFlight  = 0;
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
    ExitClass          exitClass  = ExitClass::GuestComplete;
    CounterSnapshot    counters{};
    SegmentSnapshot    segments[kSegmentCount]{};
    CheckpointSnapshot checkpoints[kCheckpointCapacity]{};
    AnomalySnapshot    anomalies[static_cast<std::size_t>(AnomalyId::CheckpointSequence) + 1]{};
    bool               transitionAttributionValid = true;
};

class Observer final
{
public:
    Observer() noexcept;
    static Observer& Instance() noexcept;

    StartStatus    Start(const StartOptions& options) noexcept;
    FinalizeStatus Finalize(ExitClass exitClass = ExitClass::GuestComplete) noexcept;

    CheckpointStatus RecordSegment(std::uint32_t         segmentIndex,
                                   const SnapshotFields& fields) noexcept;
    CheckpointStatus RecordCheckpoint(std::uint32_t         segmentIndex,
                                      std::uint32_t         markIndex,
                                      const SnapshotFields& fields) noexcept;

    void Snapshot(ObserverSnapshot& result) const noexcept;

#ifdef REREVVED_NATIVE_RENDERER_COVERAGE_TESTING
    bool SetCounterForTest(std::uint32_t segment,
                           std::uint32_t operation,
                           std::uint32_t domain,
                           std::uint32_t site,
                           std::uint64_t value) noexcept;
    void SetInFlightForTest(std::uint32_t value) noexcept;
    void RecordSiteFixedValueForTest(std::uint32_t siteIndex,
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
        std::atomic<std::uint64_t> frameSequence{ 0 };
        std::atomic<std::uint32_t> validFields{ 0 };
        std::atomic<bool>          gameplayActive{ false };
        std::atomic<bool>          interfaceUpdate{ false };
        std::atomic<std::int32_t>  activePlayer{ -1 };
        std::atomic<std::uint32_t> humanPlayerMask{ 0 };
        std::atomic<bool>          turnOwnerKnown{ false };
        std::atomic<bool>          humanTurn{ false };
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

    static Observer storage;

    void                       resetForStart() noexcept;
    void                       recordSiteHot(std::uint32_t siteIndex, std::int64_t value) noexcept;
    IncrementResult            incrementSaturating(std::atomic<std::uint64_t>& counter) noexcept;
    void                       recordIncrementResult(IncrementResult result) noexcept;
    void                       incrementAggregate(std::atomic<std::uint64_t>& counter) noexcept;
    bool                       beginAdmission() noexcept;
    void                       endAdmission() noexcept;
    void                       recordAnomaly(AnomalyId id) noexcept;
    static void                storeSnapshot(CheckpointSlot& slot, std::uint32_t segmentIndex, const SnapshotFields& fields) noexcept;
    static void                loadSnapshot(const CheckpointSlot& slot,
                                            SnapshotFields&       fields) noexcept;
    std::atomic<bool>          started{ false };
    std::atomic<bool>          enabled{ false };
    std::atomic<bool>          finalized{ false };
    std::atomic<bool>          finalizing{ false };
    std::atomic<bool>          incomplete{ false };
    std::atomic<std::uint32_t> activeSegment{ 0 };
    std::atomic<std::uint32_t> nextCheckpointMark{ 0 };
    std::atomic<std::uint32_t> inFlight{ 0 };

    std::atomic<std::uint64_t>
                               counters[kSegmentCount][kOperationCount][kValueDomainCount][kHookSiteCount]{};
    std::atomic<std::uint64_t> anomalyCounts[static_cast<std::size_t>(AnomalyId::CheckpointSequence) + 1]{};

    CheckpointSlot checkpoints[kCheckpointCapacity]{};
    CheckpointSlot segments[kSegmentCount]{};

    std::atomic<std::uint64_t> saturatedFailures{ 0 };
    std::atomic<std::uint64_t> rejectedInFlight{ 0 };
    std::atomic<bool>          transitionAttributionValid{ true };

    ExitClass exitClass           = ExitClass::GuestComplete;
    bool      recoveredIncomplete = false;
    char      runId[32]{};
    char      transitionId[32]{};
    char      inputDigest[129]{};
    char      outputDirectory[1024]{};
    char      outputRoot[1024]{};
};

// Process-wide wrappers keep generated code independent from observer object
// ownership and preserve the required zero-allocation call boundary.
StartStatus      Start(const StartOptions& options) noexcept;
FinalizeStatus   Finalize(ExitClass exitClass = ExitClass::GuestComplete) noexcept;
CheckpointStatus RecordCheckpoint(std::uint32_t         segmentIndex,
                                  std::uint32_t         markIndex,
                                  const SnapshotFields& fields) noexcept;
CheckpointStatus RecordSegment(std::uint32_t         segmentIndex,
                               const SnapshotFields& fields) noexcept;
void             Snapshot(ObserverSnapshot& result) noexcept;

} // namespace rerevved::native_renderer

#endif
