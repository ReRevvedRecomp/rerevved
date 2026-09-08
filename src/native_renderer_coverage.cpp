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

const char* ExpectedInputDigest() noexcept
{
    return rerevved::native_renderer::generated::kInputSha256;
}

std::uint32_t GeneratedSiteAddress(std::size_t site) noexcept
{
    static constexpr std::uint32_t addresses[] = {
        rerevved::native_renderer::generated::kSiteAddress82303E3C,
        rerevved::native_renderer::generated::kSiteAddress82303E8C,
    };
    return addresses[site];
}

const char* GeneratedDomainId(std::size_t domain) noexcept
{
    static constexpr const char* ids[] = {
        rerevved::native_renderer::generated::kPrimitiveDomainId,
        rerevved::native_renderer::generated::kUnknownDomainId,
    };
    return ids[domain];
}

std::size_t StringLength(const char* value, std::size_t limit) noexcept
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

bool Equal(const char* left, const char* right) noexcept
{
    if (left == nullptr || right == nullptr)
    {
        return left == right;
    }
    const std::size_t left_length  = StringLength(left, 2048);
    const std::size_t right_length = StringLength(right, 2048);
    return left_length == right_length &&
           std::memcmp(left, right, left_length) == 0;
}

bool CopyString(char* destination, std::size_t capacity, const char* source) noexcept
{
    if (destination == nullptr || source == nullptr || capacity == 0)
    {
        return false;
    }
    const std::size_t length = StringLength(source, capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

bool IsDigitString(const char* value, std::size_t begin, std::size_t end) noexcept
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

bool ValidRunId(const char* value) noexcept
{
    if (StringLength(value, 64) != 21 || value == nullptr)
    {
        return false;
    }
    return std::memcmp(value, "NRD-RUN-", 8) == 0 &&
           IsDigitString(value, 8, 16) && value[16] == '-' &&
           IsDigitString(value, 17, 21);
}

bool ValidTransitionId(const char* value) noexcept
{
    if (StringLength(value, 64) != 14 || value == nullptr)
    {
        return false;
    }
    if (std::memcmp(value, "NRD-TRANS-", 10) != 0 ||
        !IsDigitString(value, 10, 14))
    {
        return false;
    }
    const unsigned number = static_cast<unsigned>(value[10] - '0') * 1000u +
                            static_cast<unsigned>(value[11] - '0') * 100u +
                            static_cast<unsigned>(value[12] - '0') * 10u +
                            static_cast<unsigned>(value[13] - '0');
    return number >= 1u && number <= 11u;
}

bool HasParentComponent(const char* path) noexcept
{
    if (path == nullptr)
    {
        return true;
    }
    const std::size_t length = StringLength(path, kPathCapacity);
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

bool PathComponentEqual(const std::filesystem::path& left,
                        const std::filesystem::path& right) noexcept
{
#ifdef _WIN32
    std::string left_text  = left.string();
    std::string right_text = right.string();
    if (left_text.size() != right_text.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left_text.size(); ++i)
    {
        const char left_char  = left_text[i] >= 'A' && left_text[i] <= 'Z'
                                    ? static_cast<char>(left_text[i] - 'A' + 'a')
                                    : left_text[i];
        const char right_char = right_text[i] >= 'A' && right_text[i] <= 'Z'
                                    ? static_cast<char>(right_text[i] - 'A' + 'a')
                                    : right_text[i];
        if (left_char != right_char)
        {
            return false;
        }
    }
    return true;
#else
    return left == right;
#endif
}

bool IsContained(const std::filesystem::path& child,
                 const std::filesystem::path& root) noexcept
{
    auto root_it  = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it)
    {
        if (child_it == child.end() || !PathComponentEqual(*root_it, *child_it))
        {
            return false;
        }
    }
    return true;
}

void WriteJsonString(std::ostream& output, const char* value)
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

const char* ExitClassName(rerevved::native_renderer::ExitClass value) noexcept
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

const char* AnomalyName(std::size_t index) noexcept
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

Observer Observer::storage_{};

static_assert(sizeof(Observer) == kObserverByteBudget,
              "observer fixed-memory budget drifted");

Observer& Observer::Instance() noexcept
{
    return storage_;
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
                    counters_[segment][operation][domain][site].store(
                        0, std::memory_order_relaxed);
                }
            }
        }
    }
    for (std::size_t id = 0; id <= kMaxAnomaly; ++id)
    {
        anomaly_counts_[id].store(0, std::memory_order_relaxed);
    }
}

void Observer::ResetForStart() noexcept
{
    enabled_.store(false, std::memory_order_release);
    finalized_.store(false, std::memory_order_relaxed);
    finalizing_.store(false, std::memory_order_relaxed);
    incomplete_.store(false, std::memory_order_relaxed);
    active_segment_.store(0, std::memory_order_relaxed);
    next_checkpoint_mark_.store(0, std::memory_order_relaxed);
    in_flight_.store(0, std::memory_order_relaxed);
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    counters_[segment][operation][domain][site].store(
                        0, std::memory_order_relaxed);
                }
            }
        }
    }
    for (std::size_t id = 0; id <= kMaxAnomaly; ++id)
    {
        anomaly_counts_[id].store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kCheckpointCapacity; ++i)
    {
        checkpoints_[i].accepted.store(false, std::memory_order_relaxed);
        checkpoints_[i].segment.store(0, std::memory_order_relaxed);
        checkpoints_[i].frame_sequence.store(0, std::memory_order_relaxed);
        checkpoints_[i].valid_fields.store(0, std::memory_order_relaxed);
        checkpoints_[i].gameplay_active.store(false, std::memory_order_relaxed);
        checkpoints_[i].interface_update.store(false, std::memory_order_relaxed);
        checkpoints_[i].active_player.store(-1, std::memory_order_relaxed);
        checkpoints_[i].human_player_mask.store(0, std::memory_order_relaxed);
        checkpoints_[i].turn_owner_known.store(false, std::memory_order_relaxed);
        checkpoints_[i].human_turn.store(false, std::memory_order_relaxed);
        checkpoints_[i].available.store(false, std::memory_order_relaxed);
        checkpoints_[i].civilization.store(0, std::memory_order_relaxed);
        checkpoints_[i].era.store(0, std::memory_order_relaxed);
        checkpoints_[i].year.store(0, std::memory_order_relaxed);
        checkpoints_[i].turn.store(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < kSegmentCount; ++i)
    {
        segments_[i].accepted.store(false, std::memory_order_relaxed);
        segments_[i].segment.store(0, std::memory_order_relaxed);
        segments_[i].frame_sequence.store(0, std::memory_order_relaxed);
        segments_[i].valid_fields.store(0, std::memory_order_relaxed);
        segments_[i].gameplay_active.store(false, std::memory_order_relaxed);
        segments_[i].interface_update.store(false, std::memory_order_relaxed);
        segments_[i].active_player.store(-1, std::memory_order_relaxed);
        segments_[i].human_player_mask.store(0, std::memory_order_relaxed);
        segments_[i].turn_owner_known.store(false, std::memory_order_relaxed);
        segments_[i].human_turn.store(false, std::memory_order_relaxed);
        segments_[i].available.store(false, std::memory_order_relaxed);
        segments_[i].civilization.store(0, std::memory_order_relaxed);
        segments_[i].era.store(0, std::memory_order_relaxed);
        segments_[i].year.store(0, std::memory_order_relaxed);
        segments_[i].turn.store(0, std::memory_order_relaxed);
    }
    saturated_failures_.store(0, std::memory_order_relaxed);
    rejected_in_flight_.store(0, std::memory_order_relaxed);
    transition_attribution_valid_.store(true, std::memory_order_relaxed);
    exit_class_ = ExitClass::GuestComplete;
}

StartStatus Observer::Start(const StartOptions& options) noexcept
{
    if (started_.load(std::memory_order_acquire))
    {
        return finalized_.load(std::memory_order_acquire)
                   ? StartStatus::AlreadyFinalized
                   : StartStatus::AlreadyStarted;
    }
    bool expected_started = false;
    if (!started_.compare_exchange_strong(expected_started, true, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return StartStatus::AlreadyStarted;
    }
    if (!ValidRunId(options.run_id))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidRunId;
    }
    if (!ValidTransitionId(options.transition_id))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidTransitionId;
    }
    if (!Equal(options.input_digest, ExpectedInputDigest()))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidInputDigest;
    }
    if (!options.xenos_enabled)
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidXenos;
    }
    if (!options.rov_enabled)
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidRov;
    }
    if (StringLength(options.output_directory, kPathCapacity) == 0 ||
        HasParentComponent(options.output_directory))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }

    std::error_code             error;
    const std::filesystem::path output_path(options.output_directory);
    const std::filesystem::path canonical_output =
        std::filesystem::weakly_canonical(output_path, error);
    if (error || !std::filesystem::is_directory(canonical_output, error) || error)
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    std::filesystem::path canonical_root;
    if (options.output_root != nullptr && options.output_root[0] != '\0')
    {
        if (StringLength(options.output_root, kPathCapacity) == 0 ||
            HasParentComponent(options.output_root))
        {
            started_.store(false, std::memory_order_release);
            return StartStatus::InvalidOutputRoot;
        }
        canonical_root = std::filesystem::weakly_canonical(
            std::filesystem::path(options.output_root), error);
        if (error || !std::filesystem::is_directory(canonical_root, error) || error ||
            !IsContained(canonical_output, canonical_root))
        {
            started_.store(false, std::memory_order_release);
            return StartStatus::InvalidOutputRoot;
        }
    }

    if (!CopyString(run_id_, sizeof(run_id_), options.run_id) ||
        !CopyString(transition_id_, sizeof(transition_id_), options.transition_id) ||
        !CopyString(input_digest_, sizeof(input_digest_), options.input_digest) ||
        !CopyString(output_directory_, sizeof(output_directory_), canonical_output.string().c_str()))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    if (!canonical_root.empty() &&
        !CopyString(output_root_, sizeof(output_root_), canonical_root.string().c_str()))
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputRoot;
    }

    ResetForStart();
    const std::filesystem::path temporary_path =
        std::filesystem::path(output_directory_) / "coverage.json.tmp";
    recovered_incomplete_ = std::filesystem::exists(temporary_path, error) && !error;
    const std::filesystem::path sentinel_path =
        std::filesystem::path(output_directory_) / "coverage.incomplete";
    std::ofstream sentinel(sentinel_path, std::ios::binary | std::ios::trunc);
    if (!sentinel)
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    sentinel << (recovered_incomplete_ ? "recovered\n" : "incomplete\n");
    if (!sentinel)
    {
        started_.store(false, std::memory_order_release);
        return StartStatus::InvalidOutputDirectory;
    }
    enabled_.store(true, std::memory_order_release);
    return StartStatus::Accepted;
}

Observer::IncrementResult Observer::IncrementSaturating(
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

void Observer::RecordIncrementResult(IncrementResult result) noexcept
{
    if (result == IncrementResult::Saturated)
    {
        if (IncrementSaturating(saturated_failures_) !=
            IncrementResult::Incremented)
        {
            saturated_failures_.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
        const IncrementResult anomaly_result = IncrementSaturating(
            anomaly_counts_[static_cast<std::size_t>(AnomalyId::CounterSaturated)]);
        if (anomaly_result != IncrementResult::Incremented)
        {
            rejected_in_flight_.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
    else if (result == IncrementResult::Busy)
    {
        if (IncrementSaturating(rejected_in_flight_) !=
            IncrementResult::Incremented)
        {
            rejected_in_flight_.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
}

void Observer::IncrementAggregate(
    std::atomic<std::uint64_t>& counter) noexcept
{
    RecordIncrementResult(IncrementSaturating(counter));
}

bool Observer::BeginAdmission() noexcept
{
    if (!enabled_.load(std::memory_order_acquire))
    {
        return false;
    }
    std::uint32_t current = in_flight_.load(std::memory_order_relaxed);
    for (std::uint32_t attempt = 0; attempt < kCasAttempts; ++attempt)
    {
        if (current >= kMaxInFlight)
        {
            break;
        }
        if (in_flight_.compare_exchange_weak(
                current, current + 1u, std::memory_order_acquire, std::memory_order_relaxed))
        {
            if (enabled_.load(std::memory_order_acquire))
            {
                return true;
            }
            in_flight_.fetch_sub(1u, std::memory_order_release);
            return false;
        }
    }
    (void)IncrementSaturating(rejected_in_flight_);
    return false;
}

void Observer::EndAdmission() noexcept
{
    in_flight_.fetch_sub(1u, std::memory_order_release);
}

void Observer::RecordSiteHot(std::uint32_t site_index, std::int64_t value) noexcept
{
    if (!BeginAdmission())
    {
        return;
    }
    const std::size_t segment =
        active_segment_.load(std::memory_order_acquire);
    const std::size_t domain = value == 4 ? 0u : 1u;
    RecordIncrementResult(
        IncrementSaturating(counters_[segment][0][domain][site_index]));
    EndAdmission();
}

void RecordSiteFixedValue(std::uint32_t site_index, std::int64_t value) noexcept
{
    Observer& observer = Observer::Instance();
    if (!observer.enabled_.load(std::memory_order_acquire))
    {
        return;
    }
    if (site_index >= kHookSiteCount)
    {
        return;
    }
    observer.RecordSiteHot(site_index, value);
}

void Observer::RecordAnomaly(AnomalyId id) noexcept
{
    const std::size_t     index = static_cast<std::size_t>(id);
    const IncrementResult count_result =
        IncrementSaturating(anomaly_counts_[index]);
    if (count_result == IncrementResult::Saturated)
    {
        if (IncrementSaturating(saturated_failures_) !=
            IncrementResult::Incremented)
        {
            saturated_failures_.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
    else if (count_result == IncrementResult::Busy)
    {
        if (IncrementSaturating(rejected_in_flight_) !=
            IncrementResult::Incremented)
        {
            rejected_in_flight_.store(
                std::numeric_limits<std::uint64_t>::max(),
                std::memory_order_relaxed);
        }
    }
}

void Observer::StoreSnapshot(CheckpointSlot&       slot,
                             std::uint32_t         segment_index,
                             const SnapshotFields& fields) noexcept
{
    slot.segment.store(segment_index, std::memory_order_relaxed);
    slot.frame_sequence.store(fields.frame_sequence, std::memory_order_relaxed);
    slot.valid_fields.store(fields.valid_fields, std::memory_order_relaxed);
    slot.gameplay_active.store(fields.gameplay_active, std::memory_order_relaxed);
    slot.interface_update.store(fields.interface_update, std::memory_order_relaxed);
    slot.active_player.store(fields.active_player, std::memory_order_relaxed);
    slot.human_player_mask.store(fields.human_player_mask, std::memory_order_relaxed);
    slot.turn_owner_known.store(fields.turn_owner_known, std::memory_order_relaxed);
    slot.human_turn.store(fields.human_turn, std::memory_order_relaxed);
    slot.available.store(fields.available, std::memory_order_relaxed);
    slot.civilization.store(fields.civilization, std::memory_order_relaxed);
    slot.era.store(fields.era, std::memory_order_relaxed);
    slot.year.store(fields.year, std::memory_order_relaxed);
    slot.turn.store(fields.turn, std::memory_order_relaxed);
}

void Observer::LoadSnapshot(const CheckpointSlot& slot,
                            SnapshotFields&       fields) noexcept
{
    fields.frame_sequence    = slot.frame_sequence.load(std::memory_order_relaxed);
    fields.valid_fields      = slot.valid_fields.load(std::memory_order_relaxed);
    fields.gameplay_active   = slot.gameplay_active.load(std::memory_order_relaxed);
    fields.interface_update  = slot.interface_update.load(std::memory_order_relaxed);
    fields.active_player     = slot.active_player.load(std::memory_order_relaxed);
    fields.human_player_mask = slot.human_player_mask.load(std::memory_order_relaxed);
    fields.turn_owner_known  = slot.turn_owner_known.load(std::memory_order_relaxed);
    fields.human_turn        = slot.human_turn.load(std::memory_order_relaxed);
    fields.available         = slot.available.load(std::memory_order_relaxed);
    fields.civilization      = slot.civilization.load(std::memory_order_relaxed);
    fields.era               = slot.era.load(std::memory_order_relaxed);
    fields.year              = slot.year.load(std::memory_order_relaxed);
    fields.turn              = slot.turn.load(std::memory_order_relaxed);
}

CheckpointStatus Observer::RecordSegment(std::uint32_t         segment_index,
                                         const SnapshotFields& fields) noexcept
{
    if (!started_.load(std::memory_order_acquire))
    {
        return CheckpointStatus::NotStarted;
    }
    if (!enabled_.load(std::memory_order_acquire) ||
        finalizing_.load(std::memory_order_acquire))
    {
        return CheckpointStatus::Disabled;
    }
    if (segment_index >= kSegmentCount)
    {
        RecordAnomaly(AnomalyId::InvalidSegment);
        return CheckpointStatus::InvalidSegment;
    }
    CheckpointSlot& segment = segments_[segment_index];
    StoreSnapshot(segment, segment_index, fields);
    segment.accepted.store(true, std::memory_order_release);
    active_segment_.store(segment_index, std::memory_order_release);
    return CheckpointStatus::Accepted;
}

CheckpointStatus Observer::RecordCheckpoint(std::uint32_t         segment_index,
                                            std::uint32_t         mark_index,
                                            const SnapshotFields& fields) noexcept
{
    if (!started_.load(std::memory_order_acquire))
    {
        return CheckpointStatus::NotStarted;
    }
    if (!enabled_.load(std::memory_order_acquire) ||
        finalizing_.load(std::memory_order_acquire))
    {
        return CheckpointStatus::Disabled;
    }
    if (segment_index >= kSegmentCount)
    {
        RecordAnomaly(AnomalyId::InvalidSegment);
        transition_attribution_valid_.store(false, std::memory_order_release);
        RecordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidSegment;
    }
    if (mark_index >= kCheckpointCapacity)
    {
        RecordAnomaly(AnomalyId::InvalidCheckpointMark);
        transition_attribution_valid_.store(false, std::memory_order_release);
        RecordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidMark;
    }
    if (segment_index != mark_index + 1u)
    {
        transition_attribution_valid_.store(false, std::memory_order_release);
        RecordAnomaly(AnomalyId::CheckpointSequence);
        return CheckpointStatus::InvalidMark;
    }
    CheckpointSlot& checkpoint    = checkpoints_[mark_index];
    CheckpointSlot& segment       = segments_[segment_index];
    std::uint32_t   expected_mark = mark_index;
    if (!next_checkpoint_mark_.compare_exchange_strong(
            expected_mark, mark_index + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        transition_attribution_valid_.store(false, std::memory_order_release);
        RecordAnomaly(AnomalyId::CheckpointSequence);
        return checkpoint.accepted.load(std::memory_order_acquire)
                   ? CheckpointStatus::AlreadyRecorded
                   : CheckpointStatus::InvalidMark;
    }
    // Winning next_checkpoint_mark_ uniquely owns both slots. Publish the
    // segment first so an accepted checkpoint always has its mirror.
    StoreSnapshot(segment, segment_index, fields);
    StoreSnapshot(checkpoint, segment_index, fields);
    segment.accepted.store(true, std::memory_order_release);
    checkpoint.accepted.store(true, std::memory_order_release);
    active_segment_.store(segment_index, std::memory_order_release);
    return CheckpointStatus::Accepted;
}

void Observer::Snapshot(ObserverSnapshot& result) const noexcept
{
    result            = ObserverSnapshot{};
    result.started    = started_.load(std::memory_order_acquire);
    result.enabled    = enabled_.load(std::memory_order_acquire);
    result.finalized  = finalized_.load(std::memory_order_acquire);
    result.incomplete = incomplete_.load(std::memory_order_acquire);
    result.exit_class = exit_class_;
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t operation = 0; operation < kOperationCount; ++operation)
        {
            for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
            {
                for (std::size_t site = 0; site < kHookSiteCount; ++site)
                {
                    result.counters.values[segment][operation][domain][site] =
                        counters_[segment][operation][domain][site].load(
                            std::memory_order_relaxed);
                }
            }
        }
    }
    result.counters.saturated_failures =
        saturated_failures_.load(std::memory_order_relaxed);
    result.counters.rejected_in_flight =
        rejected_in_flight_.load(std::memory_order_relaxed);
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        result.segments[segment].accepted =
            segments_[segment].accepted.load(std::memory_order_acquire);
        LoadSnapshot(segments_[segment], result.segments[segment].fields);
    }
    for (std::size_t mark = 0; mark < kCheckpointCapacity; ++mark)
    {
        const CheckpointSlot& checkpoint = checkpoints_[mark];
        result.checkpoints[mark].accepted =
            checkpoint.accepted.load(std::memory_order_acquire);
        result.checkpoints[mark].segment =
            checkpoint.segment.load(std::memory_order_relaxed);
        LoadSnapshot(checkpoint, result.checkpoints[mark].fields);
    }
    for (std::size_t id = 1; id <= kMaxAnomaly; ++id)
    {
        result.anomalies[id].id = static_cast<std::uint32_t>(id);
        result.anomalies[id].count =
            anomaly_counts_[id].load(std::memory_order_relaxed);
    }
    result.transition_attribution_valid =
        transition_attribution_valid_.load(std::memory_order_acquire);
}

#ifdef REREVVED_NATIVE_RENDERER_COVERAGE_TESTING
bool Observer::SetCounterForTest(std::uint32_t segment,
                                 std::uint32_t operation,
                                 std::uint32_t domain,
                                 std::uint32_t site,
                                 std::uint64_t value) noexcept
{
    if (!enabled_.load(std::memory_order_acquire) ||
        finalizing_.load(std::memory_order_acquire) ||
        segment >= kSegmentCount || operation >= kOperationCount ||
        domain >= kValueDomainCount ||
        site >= kHookSiteCount)
    {
        return false;
    }
    counters_[segment][operation][domain][site].store(value,
                                                      std::memory_order_relaxed);
    return true;
}

void Observer::RecordSiteFixedValueForTest(std::uint32_t site_index,
                                           std::int64_t  value) noexcept
{
    if (!enabled_.load(std::memory_order_acquire) || site_index >= kHookSiteCount)
    {
        return;
    }
    RecordSiteHot(site_index, value);
}

void Observer::SetInFlightForTest(std::uint32_t value) noexcept
{
    if (!enabled_.load(std::memory_order_acquire) ||
        finalizing_.load(std::memory_order_acquire))
    {
        return;
    }
    in_flight_.store(value, std::memory_order_release);
}
#endif

namespace
{

bool WriteCoverageFile(const Observer& observer, const char* directory, const char* run_id, const char* transition_id, const char* digest, ExitClass exit_class, bool incomplete, bool recovered)
{
    ObserverSnapshot snapshot;
    observer.Snapshot(snapshot);
    const std::filesystem::path directory_path(directory);
    const std::filesystem::path temporary_path = directory_path / "coverage.json.tmp";
    const std::filesystem::path final_path     = directory_path / "coverage.json";
    std::ofstream               output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output << "{\n  \"schema\":\"rerevved.native_renderer.coverage.v1\",\n";
    output << "  \"run_id\":";
    WriteJsonString(output, run_id);
    output << ",\n  \"transition_id\":";
    WriteJsonString(output, transition_id);
    output << ",\n  \"input_digest\":";
    WriteJsonString(output, digest);
    output << ",\n  \"operation_metadata\":{\"operation_id\":";
    WriteJsonString(output,
                    rerevved::native_renderer::generated::kOperationId);
    output << ",\"runtime_join_key\":";
    WriteJsonString(output,
                    rerevved::native_renderer::generated::kRuntimeJoinKey);
    output << ",\"roles\":[";
    WriteJsonString(output,
                    rerevved::native_renderer::generated::kRoleWrapper);
    output << ',';
    WriteJsonString(
        output,
        rerevved::native_renderer::generated::kRoleLoweringBoundary);
    output << "],\"contract_ids\":[";
    WriteJsonString(output,
                    rerevved::native_renderer::generated::kContractId);
    output << "],\"hook_sites\":[";
    for (std::size_t site = 0; site < kHookSiteCount; ++site)
    {
        if (site != 0)
        {
            output << ',';
        }
        output << "{\"address\":" << GeneratedSiteAddress(site)
               << ",\"phase\":";
        WriteJsonString(output,
                        rerevved::native_renderer::generated::kValuePhase);
        output << ",\"discriminator\":";
        WriteJsonString(
            output,
            rerevved::native_renderer::generated::kPrimitiveDomainId);
        output << '}';
    }
    output << "],\"registers\":[],\"value_domains\":[{\"id\":";
    WriteJsonString(
        output, rerevved::native_renderer::generated::kPrimitiveDomainId);
    output << ",\"value\":"
           << rerevved::native_renderer::generated::kPrimitiveValue
           << ",\"selection\":";
    WriteJsonString(
        output,
        rerevved::native_renderer::generated::kSiteFixedSelection);
    output << "},{\"id\":";
    WriteJsonString(output,
                    rerevved::native_renderer::generated::kUnknownDomainId);
    output << ",\"value\":null,\"selection\":";
    WriteJsonString(
        output,
        rerevved::native_renderer::generated::kUnmappedInputSelection);
    output << "}],\n";
    output << "  \"xenos_enabled\":true,\n  \"rov_enabled\":true,\n";
    output << "  \"observer_byte_budget\":" << kObserverByteBudget
           << ",\n";
    output << "  \"exit_class\":";
    WriteJsonString(output, ExitClassName(exit_class));
    output << ",\n  \"lifetime_evaluation\":";
    WriteJsonString(output, exit_class == ExitClass::GuestComplete ? "evaluated" : "not-evaluated");
    output << ",\n  \"complete\":" << (incomplete ? "false" : "true")
           << ",\n  \"incomplete\":" << (incomplete ? "true" : "false")
           << ",\n  \"recovered_incomplete\":" << (recovered ? "true" : "false")
           << ",\n  \"transition_attribution_valid\":"
           << (snapshot.transition_attribution_valid ? "true" : "false")
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
                    WriteJsonString(
                        output, rerevved::native_renderer::generated::kOperationId);
                    output << ",\"runtime_join_key\":";
                    WriteJsonString(output,
                                    rerevved::native_renderer::generated::kRuntimeJoinKey);
                    output << ",\"contract_id\":";
                    WriteJsonString(
                        output, rerevved::native_renderer::generated::kContractId);
                    output << ",\"domain\":" << domain << ",\"domain_id\":";
                    WriteJsonString(output, GeneratedDomainId(domain));
                    output << ",\"site\":" << site
                           << ",\"site_address\":" << GeneratedSiteAddress(site)
                           << ",\"count\":"
                           << snapshot.counters.values[segment][operation][domain][site]
                           << "}";
                }
            }
        }
    }
    output << "\n  ],\n  \"counter_failures\":{\"saturated\":"
           << snapshot.counters.saturated_failures
           << ",\"rejected_in_flight\":"
           << snapshot.counters.rejected_in_flight << "},\n  \"segments\":[\n";
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        if (segment != 0)
        {
            output << ",\n";
        }
        const SnapshotFields& fields = snapshot.segments[segment].fields;
        output << "    {\"index\":" << segment << ",\"accepted\":"
               << (snapshot.segments[segment].accepted ? "true" : "false")
               << ",\"frame_sequence\":" << fields.frame_sequence
               << ",\"valid_fields\":" << fields.valid_fields
               << ",\"gameplay_active\":"
               << (fields.gameplay_active ? "true" : "false")
               << ",\"interface_update\":"
               << (fields.interface_update ? "true" : "false")
               << ",\"active_player\":" << fields.active_player
               << ",\"human_player_mask\":" << fields.human_player_mask
               << ",\"turn_owner_known\":"
               << (fields.turn_owner_known ? "true" : "false")
               << ",\"human_turn\":" << (fields.human_turn ? "true" : "false")
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
               << ",\"frame_sequence\":" << checkpoint.fields.frame_sequence
               << ",\"valid_fields\":" << checkpoint.fields.valid_fields
               << ",\"gameplay_active\":"
               << (checkpoint.fields.gameplay_active ? "true" : "false")
               << ",\"interface_update\":"
               << (checkpoint.fields.interface_update ? "true" : "false")
               << ",\"active_player\":" << checkpoint.fields.active_player
               << ",\"human_player_mask\":"
               << checkpoint.fields.human_player_mask
               << ",\"turn_owner_known\":"
               << (checkpoint.fields.turn_owner_known ? "true" : "false")
               << ",\"human_turn\":"
               << (checkpoint.fields.human_turn ? "true" : "false")
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
        WriteJsonString(output, AnomalyName(id));
        output << ",\"count\":" << snapshot.anomalies[id].count << "}";
    }
    output << "\n  ]\n}\n";
    output.close();
    if (!output)
    {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(final_path, error);
    error.clear();
    std::filesystem::rename(temporary_path, final_path, error);
    if (error)
    {
        return false;
    }
    return true;
}

} // namespace

FinalizeStatus Observer::Finalize(ExitClass exit_class) noexcept
{
    if (!started_.load(std::memory_order_acquire))
    {
        return FinalizeStatus::NotStarted;
    }
    if (finalized_.load(std::memory_order_acquire))
    {
        return FinalizeStatus::AlreadyFinalized;
    }
    if (incomplete_.load(std::memory_order_acquire))
    {
        return FinalizeStatus::Incomplete;
    }
    bool expected_finalizing = false;
    if (!finalizing_.compare_exchange_strong(expected_finalizing, true, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return FinalizeStatus::Busy;
    }
    enabled_.store(false, std::memory_order_release);
    active_segment_.store(static_cast<std::uint32_t>(kSegmentCount - 1u),
                          std::memory_order_release);
    bool drained = false;
    for (std::size_t spin = 0; spin < kFinalizeSpinLimit; ++spin)
    {
        if (in_flight_.load(std::memory_order_acquire) == 0)
        {
            drained = true;
            break;
        }
    }
    exit_class_           = exit_class;
    const bool incomplete = !drained;
    incomplete_.store(incomplete, std::memory_order_release);
    if (!drained)
    {
        RecordAnomaly(AnomalyId::FinalizationDrainTimeout);
        finalizing_.store(false, std::memory_order_release);
        return FinalizeStatus::Incomplete;
    }
    if (!WriteCoverageFile(*this, output_directory_, run_id_, transition_id_, input_digest_, exit_class, incomplete, recovered_incomplete_))
    {
        incomplete_.store(true, std::memory_order_release);
        finalizing_.store(false, std::memory_order_release);
        return FinalizeStatus::WriteFailure;
    }
    finalized_.store(true, std::memory_order_release);
    finalizing_.store(false, std::memory_order_release);
    if (!incomplete)
    {
        std::error_code error;
        std::filesystem::remove(
            std::filesystem::path(output_directory_) / "coverage.incomplete",
            error);
    }
    return FinalizeStatus::Accepted;
}

StartStatus Start(const StartOptions& options) noexcept
{
    return Observer::Instance().Start(options);
}

FinalizeStatus Finalize(ExitClass exit_class) noexcept
{
    return Observer::Instance().Finalize(exit_class);
}

CheckpointStatus RecordCheckpoint(std::uint32_t         segment_index,
                                  std::uint32_t         mark_index,
                                  const SnapshotFields& fields) noexcept
{
    return Observer::Instance().RecordCheckpoint(segment_index, mark_index, fields);
}

CheckpointStatus RecordSegment(std::uint32_t         segment_index,
                               const SnapshotFields& fields) noexcept
{
    return Observer::Instance().RecordSegment(segment_index, fields);
}

void Snapshot(ObserverSnapshot& result) noexcept
{
    Observer::Instance().Snapshot(result);
}

} // namespace rerevved::native_renderer
