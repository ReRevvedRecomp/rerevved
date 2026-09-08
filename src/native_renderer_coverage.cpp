#include "native_renderer_coverage.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "native_renderer_coverage_hooks.inc"

static_assert(rerevved::native_renderer::generated::kObserverByteBudget ==
                  rerevved::native_renderer::kObserverByteBudget,
              "generated observer byte budget drifted");
static_assert(rerevved::native_renderer::generated::kObserverSegmentCount ==
                  rerevved::native_renderer::kSegmentCount,
              "generated observer segment capacity drifted");
static_assert(rerevved::native_renderer::generated::kOperationCount ==
                  rerevved::native_renderer::kOperationCount,
              "generated operation count drifted");
static_assert(rerevved::native_renderer::generated::kValueDomainCount ==
                  rerevved::native_renderer::kValueDomainCount,
              "generated value domain count drifted");
static_assert(rerevved::native_renderer::generated::kHookSiteCount ==
                  rerevved::native_renderer::kHookSiteCount,
              "generated hook site count drifted");
static_assert(rerevved::native_renderer::generated::kCounterRowsPerSegment ==
                  rerevved::native_renderer::kOperationCount *
                      rerevved::native_renderer::kValueDomainCount *
                      rerevved::native_renderer::kHookSiteCount,
              "generated counter rows per segment drifted");
static_assert(rerevved::native_renderer::generated::kSegmentCounterRowCount ==
                  rerevved::native_renderer::kSegmentCount *
                      rerevved::native_renderer::generated::kCounterRowsPerSegment,
              "generated segmented counter row count drifted");

namespace
{

using rerevved::native_renderer::AnomalyId;
using rerevved::native_renderer::Observer;

constexpr std::size_t kMaxAnomaly =
    static_cast<std::size_t>(AnomalyId::CheckpointSequence);
constexpr std::size_t   kPathCapacity      = 1024;
constexpr std::size_t   kFinalizeSpinLimit = 100000;
constexpr std::uint32_t kCasAttempts       = 4;
static_assert(kCasAttempts > 0, "hot-path CAS attempts must be bounded");

const char* expectedInputDigest() noexcept
{
    return rerevved::native_renderer::generated::kInputSha256;
}

std::uint32_t generatedSiteAddress(std::size_t site) noexcept
{
    static constexpr std::uint32_t addresses[] = {
        rerevved::native_renderer::generated::kSiteAddress82303E3C,
        rerevved::native_renderer::generated::kSiteAddress82303E8C,
    };
    return addresses[site];
}

const char* generatedDomainId(std::size_t domain) noexcept
{
    static constexpr const char* ids[] = {
        rerevved::native_renderer::generated::kPrimitiveDomainId,
        rerevved::native_renderer::generated::kUnknownDomainId,
    };
    return ids[domain];
}

std::size_t stringLength(const char* value, std::size_t limit) noexcept
{
    if (value == nullptr)
    {
        return 0;
    }
    std::size_t length = 0;
    while (length < limit && value[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool equal(const char* left, const char* right) noexcept
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }
    const std::size_t leftLength  = stringLength(left, 2048);
    const std::size_t rightLength = stringLength(right, 2048);
    return leftLength == rightLength &&
           std::memcmp(left, right, leftLength) == 0;
}

bool copyString(char* destination, std::size_t capacity, const char* source) noexcept
{
    if (destination == nullptr || source == nullptr || capacity == 0)
    {
        return false;
    }
    const std::size_t length = stringLength(source, capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

bool isDigitString(const char* value, std::size_t begin, std::size_t end) noexcept
{
    for (std::size_t i = begin; i < end; ++i)
    {
        if (value[i] < '0' || value[i] > '9')
        {
            return false;
        }
    }
    return true;
}

bool validRunId(const char* value) noexcept
{
    if (stringLength(value, 64) != 21 || value == nullptr)
    {
        return false;
    }
    return std::memcmp(value, "NRD-RUN-", 8) == 0 &&
           isDigitString(value, 8, 16) && value[16] == '-' &&
           isDigitString(value, 17, 21);
}

bool validTransitionId(const char* value) noexcept
{
    if (stringLength(value, 64) != 14 || value == nullptr)
    {
        return false;
    }
    if (std::memcmp(value, "NRD-TRANS-", 10) != 0 ||
        !isDigitString(value, 10, 14))
    {
        return false;
    }
    const unsigned number = static_cast<unsigned>(value[10] - '0') * 1000u +
                            static_cast<unsigned>(value[11] - '0') * 100u +
                            static_cast<unsigned>(value[12] - '0') * 10u +
                            static_cast<unsigned>(value[13] - '0');
    return number >= 1u && number <= 11u;
}

bool hasParentComponent(const char* path) noexcept
{
    if (path == nullptr)
    {
        return true;
    }
    const std::size_t length = stringLength(path, kPathCapacity);
    std::size_t       begin  = 0;
    for (std::size_t i = 0; i <= length; ++i)
    {
        if (i != length && path[i] != '/' && path[i] != '\\')
        {
            continue;
        }
        if (i - begin == 2 && path[begin] == '.' && path[begin + 1] == '.')
        {
            return true;
        }
        begin = i + 1;
    }
    return false;
}

bool pathComponentEqual(const std::filesystem::path& left,
                        const std::filesystem::path& right) noexcept
{
#ifdef _WIN32
    std::string leftText  = left.string();
    std::string rightText = right.string();
    if (leftText.size() != rightText.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < leftText.size(); ++i)
    {
        const char leftChar  = leftText[i] >= 'A' && leftText[i] <= 'Z'
                                   ? static_cast<char>(leftText[i] - 'A' + 'a')
                                   : leftText[i];
        const char rightChar = rightText[i] >= 'A' && rightText[i] <= 'Z'
                                   ? static_cast<char>(rightText[i] - 'A' + 'a')
                                   : rightText[i];
        if (leftChar != rightChar)
        {
            return false;
        }
    }
    return true;
#else
    return left == right;
#endif
}

bool isContained(const std::filesystem::path& child,
                 const std::filesystem::path& root) noexcept
{
    auto rootIt  = root.begin();
    auto childIt = child.begin();
    for (; rootIt != root.end(); ++rootIt, ++childIt)
    {
        if (childIt == child.end() || !pathComponentEqual(*rootIt, *childIt))
        {
            return false;
        }
    }
    return true;
}

void writeJsonString(std::ostream& output, const char* value)
{
    output.put('"');
    if (value != nullptr)
    {
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(value);
             *cursor != 0;
             ++cursor)
        {
            switch (*cursor)
            {
                case '"':
                    output << "\\\"";
                    break;
                case '\\':
                    output << "\\\\";
                    break;
                case '\n':
                    output << "\\n";
                    break;
                case '\r':
                    output << "\\r";
                    break;
                case '\t':
                    output << "\\t";
                    break;
                default:
                    if (*cursor < 0x20)
                    {
                        output << "\\u00";
                        const char hex[] = "0123456789abcdef";
                        output.put(hex[*cursor >> 4]);
                        output.put(hex[*cursor & 0xf]);
                    }
                    else
                    {
                        output.put(static_cast<char>(*cursor));
                    }
                    break;
            }
        }
    }
    output.put('"');
}

const char* exitClassName(rerevved::native_renderer::ExitClass value) noexcept
{
    switch (value)
    {
        case rerevved::native_renderer::ExitClass::GuestComplete:
            return "guest_complete";
        case rerevved::native_renderer::ExitClass::WindowClose:
            return "window_close";
        case rerevved::native_renderer::ExitClass::Shutdown:
            return "shutdown";
    }
    return "shutdown";
}

const char* anomalyName(std::size_t index) noexcept
{
    static constexpr const char* names[kMaxAnomaly + 1] = {
        "none",
        "unused",
        "unused",
        "unused",
        "unused",
        "unused",
        "unused",
        "unused",
        "unused",
        "invalid_segment",
        "invalid_checkpoint_mark",
        "counter_saturated",
        "unused",
        "unused",
        "finalization_drain_timeout",
        "checkpoint_sequence",
    };
    return names[index];
}

} // namespace

namespace rerevved::native_renderer
{

Observer Observer::storage{};

static_assert(sizeof(Observer) == kObserverByteBudget,
              "observer fixed-memory budget drifted");

Observer& Observer::Instance() noexcept
{
    return storage;
}

Observer::Observer() noexcept
{
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    counters[segment][operation][domain][site].store(
                        0, std::memory_order_relaxed);
                }
            }
        }
    }
    for (std::size_t id = 0; id <= kMaxAnomaly; ++id)
    {
        anomalyCounts[id].store(0, std::memory_order_relaxed);
    }
}

void Observer::resetForStart() noexcept
{
    enabled.store(false, std::memory_order_release);
    finalized.store(false, std::memory_order_relaxed);
    finalizing.store(false, std::memory_order_relaxed);
    incomplete.store(false, std::memory_order_relaxed);
    activeSegment.store(0, std::memory_order_relaxed);
    nextCheckpointMark.store(0, std::memory_order_relaxed);
    inFlight.store(0, std::memory_order_relaxed);
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    counters[segment][operation][domain][site].store(
                        0, std::memory_order_relaxed);
                }
            }
        }
    }
    for (std::size_t id = 0; id <= kMaxAnomaly; ++id)
    {
        anomalyCounts[id].store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kCheckpointCapacity; ++i)
    {
        checkpoints[i].accepted.store(false, std::memory_order_relaxed);
        checkpoints[i].segment.store(0, std::memory_order_relaxed);
        checkpoints[i].frameSequence.store(0, std::memory_order_relaxed);
        checkpoints[i].validFields.store(0, std::memory_order_relaxed);
        checkpoints[i].gameplayActive.store(false, std::memory_order_relaxed);
        checkpoints[i].interfaceUpdate.store(false, std::memory_order_relaxed);
        checkpoints[i].activePlayer.store(-1, std::memory_order_relaxed);
        checkpoints[i].humanPlayerMask.store(0, std::memory_order_relaxed);
        checkpoints[i].turnOwnerKnown.store(false, std::memory_order_relaxed);
        checkpoints[i].humanTurn.store(false, std::memory_order_relaxed);
        checkpoints[i].available.store(false, std::memory_order_relaxed);
        checkpoints[i].civilization.store(0, std::memory_order_relaxed);
        checkpoints[i].era.store(0, std::memory_order_relaxed);
        checkpoints[i].year.store(0, std::memory_order_relaxed);
        checkpoints[i].turn.store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kSegmentCount; ++i)
    {
        segments[i].accepted.store(false, std::memory_order_relaxed);
        segments[i].segment.store(0, std::memory_order_relaxed);
        segments[i].frameSequence.store(0, std::memory_order_relaxed);
        segments[i].validFields.store(0, std::memory_order_relaxed);
        segments[i].gameplayActive.store(false, std::memory_order_relaxed);
        segments[i].interfaceUpdate.store(false, std::memory_order_relaxed);
        segments[i].activePlayer.store(-1, std::memory_order_relaxed);
        segments[i].humanPlayerMask.store(0, std::memory_order_relaxed);
        segments[i].turnOwnerKnown.store(false, std::memory_order_relaxed);
        segments[i].humanTurn.store(false, std::memory_order_relaxed);
        segments[i].available.store(false, std::memory_order_relaxed);
        segments[i].civilization.store(0, std::memory_order_relaxed);
        segments[i].era.store(0, std::memory_order_relaxed);
        segments[i].year.store(0, std::memory_order_relaxed);
        segments[i].turn.store(0, std::memory_order_relaxed);
    }
    saturatedFailures.store(0, std::memory_order_relaxed);
    rejectedInFlight.store(0, std::memory_order_relaxed);
    transitionAttributionValid.store(true, std::memory_order_relaxed);
    exitClass = ExitClass::GuestComplete;
}

StartStatus Observer::Start(const StartOptions& options) noexcept
{
    if (started.load(std::memory_order_acquire))
    {
        return finalized.load(std::memory_order_acquire)
                   ? StartStatus::AlreadyFinalized
                   : StartStatus::AlreadyStarted;
    }
    bool expectedStarted = false;
    if (!started.compare_exchange_strong(expectedStarted, true, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return StartStatus::AlreadyStarted;
    }
    if (!validRunId(options.runId))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidRunId;
    }
    if (!validTransitionId(options.transitionId))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidTransitionId;
    }
    if (!equal(options.inputDigest, expectedInputDigest()))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidInputDigest;
    }
    if (!options.xenosEnabled)
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidXenos;
    }
    if (!options.rovEnabled)
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidRov;
    }
    if (stringLength(options.outputDirectory, kPathCapacity) == 0 ||
        hasParentComponent(options.outputDirectory))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }

    std::error_code             error;
    const std::filesystem::path outputPath(options.outputDirectory);
    const std::filesystem::path canonicalOutput =
        std::filesystem::weakly_canonical(outputPath, error);
    if (error || !std::filesystem::is_directory(canonicalOutput, error) || error)
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    std::filesystem::path canonicalRoot;
    if (options.outputRoot != nullptr && options.outputRoot[0] != '\0')
    {
        if (stringLength(options.outputRoot, kPathCapacity) == 0 ||
            hasParentComponent(options.outputRoot))
        {
            started.store(false, std::memory_order_release);
            return StartStatus::InvalidOutputRoot;
        }
        canonicalRoot = std::filesystem::weakly_canonical(
            std::filesystem::path(options.outputRoot), error);
        if (error || !std::filesystem::is_directory(canonicalRoot, error) || error ||
            !isContained(canonicalOutput, canonicalRoot))
        {
            started.store(false, std::memory_order_release);
            return StartStatus::InvalidOutputRoot;
        }
    }

    if (!copyString(runId, sizeof(runId), options.runId) ||
        !copyString(transitionId, sizeof(transitionId), options.transitionId) ||
        !copyString(inputDigest, sizeof(inputDigest), options.inputDigest) ||
        !copyString(outputDirectory, sizeof(outputDirectory), canonicalOutput.string().c_str()))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    if (!canonicalRoot.empty() &&
        !copyString(outputRoot, sizeof(outputRoot), canonicalRoot.string().c_str()))
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputRoot;
    }

    resetForStart();
    const std::filesystem::path temporaryPath =
        std::filesystem::path(outputDirectory) / "coverage.json.tmp";
    recoveredIncomplete = std::filesystem::exists(temporaryPath, error) && !error;
    const std::filesystem::path sentinelPath =
        std::filesystem::path(outputDirectory) / "coverage.incomplete";
    std::ofstream sentinel(sentinelPath, std::ios::binary | std::ios::trunc);
    if (!sentinel)
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    sentinel << (recoveredIncomplete ? "recovered\n" : "incomplete\n");
    if (!sentinel)
    {
        started.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    enabled.store(true, std::memory_order_release);
    return StartStatus::Accepted;
}

Observer::IncrementResult Observer::incrementSaturating(
    std::atomic<std::uint64_t>& counter) noexcept
{
    std::uint64_t current = counter.load(std::memory_order_relaxed);
    for (std::uint32_t attempt = 0; attempt < kCasAttempts; ++attempt)
    {
        if (current == std::numeric_limits<std::uint64_t>::max())
        {
            return IncrementResult::Saturated;
        }
        if (counter.compare_exchange_weak(current, current + 1u, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            return IncrementResult::Incremented;
        }
    }
    return IncrementResult::Busy;
}

void Observer::recordIncrementResult(IncrementResult result) noexcept
{
    if (result == IncrementResult::Saturated)
    {
        if (incrementSaturating(saturatedFailures) !=
            IncrementResult::Incremented)
        {
            saturatedFailures.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
        const IncrementResult anomalyResult = incrementSaturating(
            anomalyCounts[static_cast<std::size_t>(AnomalyId::CounterSaturated)]);
        if (anomalyResult != IncrementResult::Incremented)
        {
            rejectedInFlight.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
    else if (result == IncrementResult::Busy)
    {
        if (incrementSaturating(rejectedInFlight) !=
            IncrementResult::Incremented)
        {
            rejectedInFlight.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
}

void Observer::incrementAggregate(
    std::atomic<std::uint64_t>& counter) noexcept
{
    recordIncrementResult(incrementSaturating(counter));
}

bool Observer::beginAdmission() noexcept
{
    if (!enabled.load(std::memory_order_acquire))
    {
        return false;
    }
    std::uint32_t current = inFlight.load(std::memory_order_relaxed);
    for (std::uint32_t attempt = 0; attempt < kCasAttempts; ++attempt)
    {
        if (current >= kMaxInFlight)
        {
            break;
        }
        if (inFlight.compare_exchange_weak(
                current, current + 1u, std::memory_order_acquire, std::memory_order_relaxed))
        {
            if (enabled.load(std::memory_order_acquire))
            {
                return true;
            }
            inFlight.fetch_sub(1u, std::memory_order_release);
            return false;
        }
    }
    (void)incrementSaturating(rejectedInFlight);
    return false;
}

void Observer::endAdmission() noexcept
{
    inFlight.fetch_sub(1u, std::memory_order_release);
}

void Observer::recordSiteHot(std::uint32_t siteIndex, std::int64_t value) noexcept
{
    if (!beginAdmission())
    {
        return;
    }
    const std::size_t segment =
        activeSegment.load(std::memory_order_acquire);
    const std::size_t domain = value == 4 ? 0u : 1u;
    recordIncrementResult(
        incrementSaturating(counters[segment][0][domain][siteIndex]));
    endAdmission();
}

void RecordSiteFixedValue(std::uint32_t siteIndex, std::int64_t value) noexcept
{
    Observer& observer = Observer::Instance();
    if (!observer.enabled.load(std::memory_order_acquire))
    {
        return;
    }
    if (siteIndex >= kHookSiteCount)
    {
        return;
    }
    observer.recordSiteHot(siteIndex, value);
}

void Observer::recordAnomaly(AnomalyId id) noexcept
{
    const std::size_t     index = static_cast<std::size_t>(id);
    const IncrementResult countResult =
        incrementSaturating(anomalyCounts[index]);
    if (countResult == IncrementResult::Saturated)
    {
        if (incrementSaturating(saturatedFailures) !=
            IncrementResult::Incremented)
        {
            saturatedFailures.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
    else if (countResult == IncrementResult::Busy)
    {
        if (incrementSaturating(rejectedInFlight) !=
            IncrementResult::Incremented)
        {
            rejectedInFlight.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
}

void Observer::storeSnapshot(CheckpointSlot&       slot,
                             std::uint32_t         segmentIndex,
                             const SnapshotFields& fields) noexcept
{
    slot.segment.store(segmentIndex, std::memory_order_relaxed);
    slot.frameSequence.store(fields.frameSequence, std::memory_order_relaxed);
    slot.validFields.store(fields.validFields, std::memory_order_relaxed);
    slot.gameplayActive.store(fields.gameplayActive, std::memory_order_relaxed);
    slot.interfaceUpdate.store(fields.interfaceUpdate, std::memory_order_relaxed);
    slot.activePlayer.store(fields.activePlayer, std::memory_order_relaxed);
    slot.humanPlayerMask.store(fields.humanPlayerMask, std::memory_order_relaxed);
    slot.turnOwnerKnown.store(fields.turnOwnerKnown, std::memory_order_relaxed);
    slot.humanTurn.store(fields.humanTurn, std::memory_order_relaxed);
    slot.available.store(fields.available, std::memory_order_relaxed);
    slot.civilization.store(fields.civilization, std::memory_order_relaxed);
    slot.era.store(fields.era, std::memory_order_relaxed);
    slot.year.store(fields.year, std::memory_order_relaxed);
    slot.turn.store(fields.turn, std::memory_order_relaxed);
}

void Observer::loadSnapshot(const CheckpointSlot& slot,
                            SnapshotFields&       fields) noexcept
{
    fields.frameSequence   = slot.frameSequence.load(std::memory_order_relaxed);
    fields.validFields     = slot.validFields.load(std::memory_order_relaxed);
    fields.gameplayActive  = slot.gameplayActive.load(std::memory_order_relaxed);
    fields.interfaceUpdate = slot.interfaceUpdate.load(std::memory_order_relaxed);
    fields.activePlayer    = slot.activePlayer.load(std::memory_order_relaxed);
    fields.humanPlayerMask = slot.humanPlayerMask.load(std::memory_order_relaxed);
    fields.turnOwnerKnown  = slot.turnOwnerKnown.load(std::memory_order_relaxed);
    fields.humanTurn       = slot.humanTurn.load(std::memory_order_relaxed);
    fields.available       = slot.available.load(std::memory_order_relaxed);
    fields.civilization    = slot.civilization.load(std::memory_order_relaxed);
    fields.era             = slot.era.load(std::memory_order_relaxed);
    fields.year            = slot.year.load(std::memory_order_relaxed);
    fields.turn            = slot.turn.load(std::memory_order_relaxed);
}

CheckpointStatus Observer::RecordSegment(std::uint32_t         segmentIndex,
                                         const SnapshotFields& fields) noexcept
{
    if (!started.load(std::memory_order_acquire))
    {
        return CheckpointStatus::NotStarted;
    }
    if (!enabled.load(std::memory_order_acquire) ||
        finalizing.load(std::memory_order_acquire))
    {
        return CheckpointStatus::Disabled;
    }
    if (segmentIndex >= kSegmentCount)
    {
        recordAnomaly(AnomalyId::InvalidSegment);
        return CheckpointStatus::InvalidSegment;
    }
    CheckpointSlot& segment = segments[segmentIndex];
    storeSnapshot(segment, segmentIndex, fields);
    segment.accepted.store(true, std::memory_order_release);
    activeSegment.store(segmentIndex, std::memory_order_release);
    return CheckpointStatus::Accepted;
}

CheckpointStatus Observer::RecordCheckpoint(std::uint32_t         segmentIndex,
                                            std::uint32_t         markIndex,
                                            const SnapshotFields& fields) noexcept
{
    if (!started.load(std::memory_order_acquire))
    {
        return CheckpointStatus::NotStarted;
    }
    if (!enabled.load(std::memory_order_acquire) ||
        finalizing.load(std::memory_order_acquire))
    {
        return CheckpointStatus::Disabled;
    }
    if (segmentIndex >= kSegmentCount)
    {
        recordAnomaly(AnomalyId::InvalidSegment);
        transitionAttributionValid.store(false, std::memory_order_release);
        recordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidSegment;
    }
    if (markIndex >= kCheckpointCapacity)
    {
        recordAnomaly(AnomalyId::InvalidCheckpointMark);
        transitionAttributionValid.store(false, std::memory_order_release);
        recordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidMark;
    }
    if (segmentIndex != markIndex + 1u)
    {
        transitionAttributionValid.store(false, std::memory_order_release);
        recordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidMark;
    }
    CheckpointSlot& checkpoint   = checkpoints[markIndex];
    CheckpointSlot& segment      = segments[segmentIndex];
    std::uint32_t   expectedMark = markIndex;
    if (!nextCheckpointMark.compare_exchange_strong(
            expectedMark, markIndex + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        transitionAttributionValid.store(false, std::memory_order_release);
        recordAnomaly(AnomalyId::CheckpointSequence);
        return checkpoint.accepted.load(std::memory_order_acquire)
                   ? CheckpointStatus::AlreadyRecorded
                   : CheckpointStatus::InvalidMark;
    }
    // Winning nextCheckpointMark uniquely owns both slots. Publish the
    // segment first so an accepted checkpoint always has its mirror.
    storeSnapshot(segment, segmentIndex, fields);
    storeSnapshot(checkpoint, segmentIndex, fields);
    segment.accepted.store(true, std::memory_order_release);
    checkpoint.accepted.store(true, std::memory_order_release);
    activeSegment.store(segmentIndex, std::memory_order_release);
    return CheckpointStatus::Accepted;
}

void Observer::Snapshot(ObserverSnapshot& result) const noexcept
{
    result            = ObserverSnapshot{};
    result.started    = started.load(std::memory_order_acquire);
    result.enabled    = enabled.load(std::memory_order_acquire);
    result.finalized  = finalized.load(std::memory_order_acquire);
    result.incomplete = incomplete.load(std::memory_order_acquire);
    result.exitClass  = exitClass;
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    result.counters.values[segment][operation][domain][site] =
                        counters[segment][operation][domain][site].load(
                            std::memory_order_relaxed);
                }
            }
        }
    }
    result.counters.saturatedFailures =
        saturatedFailures.load(std::memory_order_relaxed);
    result.counters.rejectedInFlight =
        rejectedInFlight.load(std::memory_order_relaxed);
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        result.segments[segment].accepted =
            segments[segment].accepted.load(std::memory_order_acquire);
        loadSnapshot(segments[segment], result.segments[segment].fields);
    }
    for (std::size_t mark = 0; mark < kCheckpointCapacity; ++mark)
    {
        const CheckpointSlot& checkpoint = checkpoints[mark];
        result.checkpoints[mark].accepted =
            checkpoint.accepted.load(std::memory_order_acquire);
        result.checkpoints[mark].segment =
            checkpoint.segment.load(std::memory_order_relaxed);
        loadSnapshot(checkpoint, result.checkpoints[mark].fields);
    }
    for (std::size_t id = 1; id <= kMaxAnomaly; ++id)
    {
        result.anomalies[id].id = static_cast<std::uint32_t>(id);
        result.anomalies[id].count =
            anomalyCounts[id].load(std::memory_order_relaxed);
    }
    result.transitionAttributionValid =
        transitionAttributionValid.load(std::memory_order_acquire);
}

#ifdef REREVVED_NATIVE_RENDERER_COVERAGE_TESTING
bool Observer::SetCounterForTest(std::uint32_t segment,
                                 std::uint32_t operation,
                                 std::uint32_t domain,
                                 std::uint32_t site,
                                 std::uint64_t value) noexcept
{
    if (!enabled.load(std::memory_order_acquire) ||
        finalizing.load(std::memory_order_acquire) ||
        segment >= kSegmentCount || operation >= kOperationCount ||
        domain >= kValueDomainCount ||
        site >= kHookSiteCount)
    {
        return false;
    }
    counters[segment][operation][domain][site].store(value,
                                                     std::memory_order_relaxed);
    return true;
}

void Observer::RecordSiteFixedValueForTest(std::uint32_t siteIndex,
                                           std::int64_t  value) noexcept
{
    if (!enabled.load(std::memory_order_acquire) || siteIndex >= kHookSiteCount)
    {
        return;
    }
    recordSiteHot(siteIndex, value);
}

void Observer::SetInFlightForTest(std::uint32_t value) noexcept
{
    if (!enabled.load(std::memory_order_acquire) ||
        finalizing.load(std::memory_order_acquire))
    {
        return;
    }
    inFlight.store(value, std::memory_order_release);
}
#endif

namespace
{

bool writeCoverageFile(const Observer& observer, const char* directory, const char* runId, const char* transitionId, const char* digest, ExitClass exitClass, bool incomplete, bool recovered)
{
    ObserverSnapshot snapshot;
    observer.Snapshot(snapshot);
    const std::filesystem::path directoryPath(directory);
    const std::filesystem::path temporaryPath = directoryPath / "coverage.json.tmp";
    const std::filesystem::path finalPath     = directoryPath / "coverage.json";
    std::ofstream               output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output << "{\n  \"schema\":\"rerevved.native_renderer.coverage.v1\",\n";
    output << "  \"run_id\":";
    writeJsonString(output, runId);
    output << ",\n  \"transition_id\":";
    writeJsonString(output, transitionId);
    output << ",\n  \"input_digest\":";
    writeJsonString(output, digest);
    output << ",\n  \"operation_metadata\":{\"operation_id\":";
    writeJsonString(output,
                    rerevved::native_renderer::generated::kOperationId);
    output << ",\"runtime_join_key\":";
    writeJsonString(output,
                    rerevved::native_renderer::generated::kRuntimeJoinKey);
    output << ",\"roles\":[";
    writeJsonString(output,
                    rerevved::native_renderer::generated::kRoleWrapper);
    output << ',';
    writeJsonString(
        output,
        rerevved::native_renderer::generated::kRoleLoweringBoundary);
    output << "],\"contract_ids\":[";
    writeJsonString(output,
                    rerevved::native_renderer::generated::kContractId);
    output << "],\"hook_sites\":[";
    for (std::size_t site = 0; site < kHookSiteCount; ++site)
    {
        if (site != 0)
        {
            output << ',';
        }
        output << "{\"address\":" << generatedSiteAddress(site)
               << ",\"phase\":";
        writeJsonString(output,
                        rerevved::native_renderer::generated::kValuePhase);
        output << ",\"discriminator\":";
        writeJsonString(
            output,
            rerevved::native_renderer::generated::kPrimitiveDomainId);
        output << '}';
    }
    output << "],\"registers\":[],\"value_domains\":[{\"id\":";
    writeJsonString(
        output, rerevved::native_renderer::generated::kPrimitiveDomainId);
    output << ",\"value\":"
           << rerevved::native_renderer::generated::kPrimitiveValue
           << ",\"selection\":";
    writeJsonString(
        output,
        rerevved::native_renderer::generated::kSiteFixedSelection);
    output << "},{\"id\":";
    writeJsonString(output,
                    rerevved::native_renderer::generated::kUnknownDomainId);
    output << ",\"value\":null,\"selection\":";
    writeJsonString(
        output,
        rerevved::native_renderer::generated::kUnmappedInputSelection);
    output << "}],\n";
    output << "  \"xenos_enabled\":true,\n  \"rov_enabled\":true,\n";
    output << "  \"observer_byte_budget\":" << kObserverByteBudget
           << ",\n";
    output << "  \"exit_class\":";
    writeJsonString(output, exitClassName(exitClass));
    output << ",\n  \"lifetime_evaluation\":";
    writeJsonString(output, exitClass == ExitClass::GuestComplete ? "evaluated" : "not-evaluated");
    output << ",\n  \"complete\":" << (incomplete ? "false" : "true")
           << ",\n  \"incomplete\":" << (incomplete ? "true" : "false")
           << ",\n  \"recovered_incomplete\":" << (recovered ? "true" : "false")
           << ",\n  \"transition_attribution_valid\":"
           << (snapshot.transitionAttributionValid ? "true" : "false")
           << ",\n  \"counters\":[\n";
    bool first = true;
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    if (!first)
                    {
                        output << ",\n";
                    }
                    first = false;
                    output << "    {\"segment\":" << segment
                           << ",\"operation\":" << operation
                           << ",\"operation_id\":";
                    writeJsonString(
                        output, rerevved::native_renderer::generated::kOperationId);
                    output << ",\"runtime_join_key\":";
                    writeJsonString(output,
                                    rerevved::native_renderer::generated::kRuntimeJoinKey);
                    output << ",\"contract_id\":";
                    writeJsonString(
                        output, rerevved::native_renderer::generated::kContractId);
                    output << ",\"domain\":" << domain << ",\"domain_id\":";
                    writeJsonString(output, generatedDomainId(domain));
                    output << ",\"site\":" << site
                           << ",\"site_address\":" << generatedSiteAddress(site)
                           << ",\"count\":"
                           << snapshot.counters.values[segment][operation][domain][site]
                           << "}";
                }
            }
        }
    }
    output << "\n  ],\n  \"counter_failures\":{\"saturated\":"
           << snapshot.counters.saturatedFailures
           << ",\"rejected_in_flight\":"
           << snapshot.counters.rejectedInFlight << "},\n  \"segments\":[\n";
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        if (segment != 0)
        {
            output << ",\n";
        }
        const SnapshotFields& fields = snapshot.segments[segment].fields;
        output << "    {\"index\":" << segment << ",\"accepted\":"
               << (snapshot.segments[segment].accepted ? "true" : "false")
               << ",\"frame_sequence\":" << fields.frameSequence
               << ",\"valid_fields\":" << fields.validFields
               << ",\"gameplay_active\":"
               << (fields.gameplayActive ? "true" : "false")
               << ",\"interface_update\":"
               << (fields.interfaceUpdate ? "true" : "false")
               << ",\"active_player\":" << fields.activePlayer
               << ",\"human_player_mask\":" << fields.humanPlayerMask
               << ",\"turn_owner_known\":"
               << (fields.turnOwnerKnown ? "true" : "false")
               << ",\"human_turn\":" << (fields.humanTurn ? "true" : "false")
               << ",\"available\":" << (fields.available ? "true" : "false")
               << ",\"civilization\":" << fields.civilization
               << ",\"era\":" << fields.era << ",\"year\":" << fields.year
               << ",\"turn\":" << fields.turn << "}";
    }
    output << "\n  ],\n  \"checkpoints\":[\n";
    for (std::size_t mark = 0; mark < kCheckpointCapacity; ++mark)
    {
        if (mark != 0)
        {
            output << ",\n";
        }
        const CheckpointSnapshot& checkpoint = snapshot.checkpoints[mark];
        output << "    {\"mark\":" << mark << ",\"accepted\":"
               << (checkpoint.accepted ? "true" : "false")
               << ",\"segment\":" << checkpoint.segment
               << ",\"frame_sequence\":" << checkpoint.fields.frameSequence
               << ",\"valid_fields\":" << checkpoint.fields.validFields
               << ",\"gameplay_active\":"
               << (checkpoint.fields.gameplayActive ? "true" : "false")
               << ",\"interface_update\":"
               << (checkpoint.fields.interfaceUpdate ? "true" : "false")
               << ",\"active_player\":" << checkpoint.fields.activePlayer
               << ",\"human_player_mask\":"
               << checkpoint.fields.humanPlayerMask
               << ",\"turn_owner_known\":"
               << (checkpoint.fields.turnOwnerKnown ? "true" : "false")
               << ",\"human_turn\":"
               << (checkpoint.fields.humanTurn ? "true" : "false")
               << ",\"available\":" << (checkpoint.fields.available ? "true" : "false")
               << ",\"civilization\":" << checkpoint.fields.civilization
               << ",\"era\":" << checkpoint.fields.era
               << ",\"year\":" << checkpoint.fields.year
               << ",\"turn\":" << checkpoint.fields.turn << "}";
    }
    output << "\n  ],\n  \"anomalies\":[\n";
    first = true;
    for (std::size_t id = 1; id <= kMaxAnomaly; ++id)
    {
        if (snapshot.anomalies[id].count == 0)
        {
            continue;
        }
        if (!first)
        {
            output << ",\n";
        }
        first = false;
        output << "    {\"id\":" << id << ",\"name\":";
        writeJsonString(output, anomalyName(id));
        output << ",\"count\":" << snapshot.anomalies[id].count << "}";
    }
    output << "\n  ]\n}\n";
    output.close();
    if (!output)
    {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(finalPath, error);
    error.clear();
    std::filesystem::rename(temporaryPath, finalPath, error);
    if (error)
    {
        return false;
    }
    return true;
}

} // namespace

FinalizeStatus Observer::Finalize(ExitClass exitClass) noexcept
{
    if (!started.load(std::memory_order_acquire))
    {
        return FinalizeStatus::NotStarted;
    }
    if (finalized.load(std::memory_order_acquire))
    {
        return FinalizeStatus::AlreadyFinalized;
    }
    if (incomplete.load(std::memory_order_acquire))
    {
        return FinalizeStatus::Incomplete;
    }
    bool expectedFinalizing = false;
    if (!finalizing.compare_exchange_strong(expectedFinalizing, true, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return FinalizeStatus::Busy;
    }
    enabled.store(false, std::memory_order_release);
    activeSegment.store(static_cast<std::uint32_t>(kSegmentCount - 1u),
                        std::memory_order_release);
    bool drained = false;
    for (std::size_t spin = 0; spin < kFinalizeSpinLimit; ++spin)
    {
        if (inFlight.load(std::memory_order_acquire) == 0)
        {
            drained = true;
            break;
        }
    }
    this->exitClass       = exitClass;
    const bool incomplete = !drained;
    this->incomplete.store(incomplete, std::memory_order_release);
    if (!drained)
    {
        recordAnomaly(AnomalyId::FinalizationDrainTimeout);
        finalizing.store(false, std::memory_order_release);
        return FinalizeStatus::Incomplete;
    }
    if (!writeCoverageFile(*this, outputDirectory, runId, transitionId, inputDigest, exitClass, incomplete, recoveredIncomplete))
    {
        this->incomplete.store(true, std::memory_order_release);
        finalizing.store(false, std::memory_order_release);
        return FinalizeStatus::WriteFailure;
    }
    finalized.store(true, std::memory_order_release);
    finalizing.store(false, std::memory_order_release);
    if (!incomplete)
    {
        std::error_code error;
        std::filesystem::remove(
            std::filesystem::path(outputDirectory) / "coverage.incomplete",
            error);
    }
    return FinalizeStatus::Accepted;
}

StartStatus Start(const StartOptions& options) noexcept
{
    return Observer::Instance().Start(options);
}

FinalizeStatus Finalize(ExitClass exitClass) noexcept
{
    return Observer::Instance().Finalize(exitClass);
}

CheckpointStatus RecordCheckpoint(std::uint32_t         segmentIndex,
                                  std::uint32_t         markIndex,
                                  const SnapshotFields& fields) noexcept
{
    return Observer::Instance().RecordCheckpoint(segmentIndex, markIndex, fields);
}

CheckpointStatus RecordSegment(std::uint32_t         segmentIndex,
                               const SnapshotFields& fields) noexcept
{
    return Observer::Instance().RecordSegment(segmentIndex, fields);
}

void Snapshot(ObserverSnapshot& result) noexcept
{
    Observer::Instance().Snapshot(result);
}

} // namespace rerevved::native_renderer
