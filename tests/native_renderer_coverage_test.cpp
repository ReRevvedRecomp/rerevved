#include "../src/native_renderer_coverage.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace rerevved::native_renderer;

constexpr char kAcceptedInputDigest[] =
    "2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303";

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path MakeDirectory(const char* suffix)
{
    const auto      path = std::filesystem::temp_directory_path() /
                           (std::string("rerevved-b1b-") + suffix);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    Require(!error, "create temporary output directory");
    return path;
}

StartOptions Options(const std::filesystem::path& directory)
{
    static std::string output;
    output = directory.string();
    StartOptions options;
    options.run_id           = "NRD-RUN-20260829-0001";
    options.transition_id    = "NRD-TRANS-0001";
    options.input_digest     = kAcceptedInputDigest;
    options.output_directory = output.c_str();
    options.xenos_enabled    = true;
    options.rov_enabled      = true;
    return options;
}

SnapshotFields Fields()
{
    SnapshotFields fields;
    fields.frame_sequence    = 71;
    fields.valid_fields      = 0x1f;
    fields.gameplay_active   = true;
    fields.interface_update  = true;
    fields.active_player     = -1;
    fields.human_player_mask = 5;
    fields.turn_owner_known  = true;
    fields.human_turn        = false;
    fields.available         = true;
    fields.civilization      = 2;
    fields.era               = 3;
    fields.year              = 1492;
    fields.turn              = 10;
    return fields;
}

bool SameFields(const SnapshotFields& left, const SnapshotFields& right)
{
    return left.frame_sequence == right.frame_sequence &&
           left.valid_fields == right.valid_fields &&
           left.gameplay_active == right.gameplay_active &&
           left.interface_update == right.interface_update &&
           left.active_player == right.active_player &&
           left.human_player_mask == right.human_player_mask &&
           left.turn_owner_known == right.turn_owner_known &&
           left.human_turn == right.human_turn &&
           left.available == right.available &&
           left.civilization == right.civilization && left.era == right.era &&
           left.year == right.year && left.turn == right.turn;
}

void StartFresh(Observer& observer, const std::filesystem::path& directory)
{
    const StartStatus status = observer.Start(Options(directory));
    Require(status == StartStatus::Accepted, "start observer");
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "open generated coverage");
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count  = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos)
    {
        ++count;
        offset += needle.size();
    }
    return count;
}

void TestDisabledAndHooks()
{
    Require(Observer::Instance().ObjectSizeBytes() == kObserverByteBudget,
            "observer object matches fixed byte budget");
    ObserverSnapshot snapshot;
    Snapshot(snapshot);
    Require(!snapshot.enabled, "observer starts disabled");
    RecordSiteFixedValue(0, 4);
    Require(!Observer::Instance().SetCounterForTest(0, 0, 0, 0, 1),
            "disabled counter test hook is ignored");
    Snapshot(snapshot);
    Require(snapshot.counters.values[0][0][1][0] == 0,
            "disabled recorder does not count");

    const auto directory = MakeDirectory("disabled");
    StartFresh(Observer::Instance(), directory);
    RecordSiteFixedValue(0, 4);
    RecordSiteFixedValue(1, 0);
    RecordSiteFixedValue(1, 5);
    Snapshot(snapshot);
    Require(snapshot.counters.values[0][0][0][0] == 1,
            "primitive-4 value site zero count");
    Require(snapshot.counters.values[0][0][1][1] == 2,
            "unknown value domain site one count");
    Require(snapshot.counters.values[0][0][1][0] == 0,
            "primitive-4 domain excludes unknown values");
    Require(snapshot.counters.values[0][0][0][1] == 0,
            "unknown domain excludes primitive-4");
    Require(Observer::Instance().SetCounterForTest(
                0, 0, 0, 0, UINT64_MAX - 1u),
            "set saturation test counter");
    std::vector<std::thread> saturation_workers;
    for (int worker = 0; worker < 8; ++worker)
    {
        saturation_workers.emplace_back([]
                                        {
                                            RecordSiteFixedValue(0, 4);
                                        });
    }
    for (auto& worker : saturation_workers)
    {
        worker.join();
    }
    Snapshot(snapshot);
    Require(snapshot.counters.values[0][0][0][0] == UINT64_MAX,
            "counter saturates");
    Require(snapshot.counters.saturated_failures >= 1,
            "counter saturation failure is counted");
    Require(snapshot.anomalies[static_cast<std::size_t>(AnomalyId::CounterSaturated)].count >= 1,
            "counter saturation anomaly ID is recorded");
    Require(Finalize() == FinalizeStatus::Accepted, "finalize guest complete");
    RecordSiteFixedValue(0, 4);
    Snapshot(snapshot);
    Require(snapshot.counters.values[0][0][0][0] == UINT64_MAX,
            "finalized recorder is disabled");
    Require(Finalize() == FinalizeStatus::AlreadyFinalized,
            "finalize is idempotent");
    Require(Start(Options(directory)) == StartStatus::AlreadyFinalized,
            "a finalized process cannot restart another run");
}

void TestValidationAndCheckpoints()
{
    const auto   directory = MakeDirectory("validation");
    Observer     invalid;
    StartOptions options = Options(directory);
    options.run_id       = "NRD-RUN-2026X829-0001";
    Require(invalid.Start(options) == StartStatus::InvalidRunId,
            "run id range format");
    options               = Options(directory);
    options.transition_id = "NRD-TRANS-0012";
    Require(invalid.Start(options) == StartStatus::InvalidTransitionId,
            "transition id range format");
    options              = Options(directory);
    options.input_digest = "wrong";
    Require(invalid.Start(options) == StartStatus::InvalidInputDigest,
            "digest validation");
    options               = Options(directory);
    options.xenos_enabled = false;
    Require(invalid.Start(options) == StartStatus::InvalidXenos,
            "xenos validation");
    options             = Options(directory);
    options.rov_enabled = false;
    Require(invalid.Start(options) == StartStatus::InvalidRov, "rov validation");

    const auto root  = MakeDirectory("contained-root");
    const auto child = root / "capture";
    std::filesystem::create_directories(child);
    std::string root_text = root.string();
    Observer    contained;
    options             = Options(child);
    options.output_root = root_text.c_str();
    Require(contained.Start(options) == StartStatus::Accepted,
            "contained output directory accepted");
    Require(contained.Finalize() == FinalizeStatus::Accepted,
            "finalize contained output directory");
    const auto outside = MakeDirectory("outside-root");
    Observer   outside_observer;
    options             = Options(outside);
    options.output_root = root_text.c_str();
    Require(outside_observer.Start(options) == StartStatus::InvalidOutputRoot,
            "output directory escape rejected");

    Observer checkpoint_observer;
    StartFresh(checkpoint_observer, directory);

    const SnapshotFields fields = Fields();
    Require(checkpoint_observer.RecordSegment(0, fields) ==
                CheckpointStatus::Accepted,
            "first segment is accepted");
    checkpoint_observer.RecordSiteFixedValueForTest(0, 4);
    Require(checkpoint_observer.RecordCheckpoint(2, 1, fields) ==
                CheckpointStatus::InvalidMark,
            "out-of-order checkpoint is rejected");
    Require(checkpoint_observer.RecordCheckpoint(2, 0, fields) ==
                CheckpointStatus::InvalidMark,
            "checkpoint segment must equal mark plus one");
    ObserverSnapshot rejected_snapshot;
    checkpoint_observer.Snapshot(rejected_snapshot);
    Require(!rejected_snapshot.segments[1].accepted &&
                !rejected_snapshot.segments[2].accepted &&
                !rejected_snapshot.checkpoints[0].accepted &&
                !rejected_snapshot.checkpoints[1].accepted,
            "rejected checkpoints publish neither checkpoint nor segment");
    Require(checkpoint_observer.RecordCheckpoint(1, 0, fields) ==
                CheckpointStatus::Accepted,
            "first checkpoint advances to its segment");
    SnapshotFields retry_fields = fields;
    retry_fields.frame_sequence = fields.frame_sequence + 1u;
    Require(checkpoint_observer.RecordCheckpoint(1, 0, retry_fields) ==
                CheckpointStatus::AlreadyRecorded,
            "accepted checkpoint retry is rejected");
    ObserverSnapshot retry_snapshot;
    checkpoint_observer.Snapshot(retry_snapshot);
    Require(SameFields(retry_snapshot.checkpoints[0].fields, fields) &&
                SameFields(retry_snapshot.segments[1].fields, fields),
            "checkpoint retry cannot mutate either accepted row");
    checkpoint_observer.RecordSiteFixedValueForTest(1, 4);
    for (std::uint32_t mark = 1; mark < kCheckpointCapacity; ++mark)
    {
        const std::uint32_t segment = mark + 1u;
        Require(checkpoint_observer.RecordCheckpoint(segment, mark, fields) ==
                    CheckpointStatus::Accepted,
                "manual checkpoints are monotonic");
    }
    Require(checkpoint_observer.RecordSegment(7, fields) ==
                CheckpointStatus::Accepted,
            "final segment is accepted");
    checkpoint_observer.RecordSiteFixedValueForTest(0, 4);
    Require(checkpoint_observer.RecordSegment(8, fields) ==
                CheckpointStatus::InvalidSegment,
            "ninth segment is rejected");
    Require(checkpoint_observer.RecordCheckpoint(0, 6, fields) ==
                CheckpointStatus::InvalidMark,
            "seventh checkpoint is rejected");
    ObserverSnapshot snapshot;
    checkpoint_observer.Snapshot(snapshot);
    Require(!snapshot.transition_attribution_valid &&
                snapshot.anomalies[static_cast<std::size_t>(
                                       AnomalyId::CheckpointSequence)]
                        .count == 4,
            "checkpoint mismatches and retry have stable sequence anomalies");
    Require(snapshot.segments[7].accepted &&
                snapshot.segments[7].fields.frame_sequence == fields.frame_sequence,
            "segment snapshot preserves accepted gameplay fields");
    Require(snapshot.counters.values[0][0][0][0] == 1 &&
                snapshot.counters.values[1][0][0][1] == 1 &&
                snapshot.counters.values[7][0][0][0] == 1,
            "counter rows stay separated by accepted segment");
    for (std::size_t mark = 0; mark < kCheckpointCapacity; ++mark)
    {
        const std::size_t         segment_index = mark + 1u;
        const CheckpointSnapshot& saved         = snapshot.checkpoints[mark];
        const SegmentSnapshot&    segment       = snapshot.segments[segment_index];
        Require(saved.accepted && saved.segment == segment_index &&
                    segment.accepted && SameFields(saved.fields, segment.fields) &&
                    SameFields(saved.fields, fields),
                "checkpoint publishes an identical accepted segment snapshot");
    }
    Require(checkpoint_observer.Finalize() == FinalizeStatus::Accepted,
            "finalize checkpoint run");
    Require(checkpoint_observer.RecordSegment(0, fields) == CheckpointStatus::Disabled,
            "segment recording is disabled after finalization");
    Require(checkpoint_observer.RecordCheckpoint(0, 0, fields) ==
                CheckpointStatus::Disabled,
            "checkpoint recording is disabled after finalization");
}

void TestConcurrencyAndDeterminism()
{
    constexpr int kThreads = 8;
    const auto    first    = MakeDirectory("concurrency-a");
    Observer      first_observer;
    StartFresh(first_observer, first);
    constexpr int            kCalls = 2000;
    std::atomic<int>         ready{ 0 };
    std::atomic<bool>        go{ false };
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        workers.emplace_back([&first_observer, &ready, &go]
                             {
                                 ready.fetch_add(1, std::memory_order_release);
                                 while (!go.load(std::memory_order_acquire))
                                 {
                                     std::this_thread::yield();
                                 }
                                 for (int call = 0; call < kCalls; ++call)
                                 {
                                     first_observer.RecordSiteFixedValueForTest(
                                         static_cast<std::uint32_t>(call & 1), 4);
                                 }
                             });
    }
    while (ready.load(std::memory_order_acquire) != kThreads)
    {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    Require(first_observer.Finalize() == FinalizeStatus::Accepted,
            "finalization overlaps concurrent admission");
    for (auto& worker : workers)
    {
        worker.join();
    }
    ObserverSnapshot first_snapshot;
    first_observer.Snapshot(first_snapshot);
    const std::uint64_t first_count =
        first_snapshot.counters.values[0][0][0][0] +
        first_snapshot.counters.values[0][0][0][1];
    Require(first_count <= static_cast<std::uint64_t>(kThreads * kCalls),
            "concurrent admission remains bounded");
    for (int call = 0; call < 100; ++call)
    {
        first_observer.RecordSiteFixedValueForTest(0, 4);
    }
    ObserverSnapshot after_finalize;
    first_observer.Snapshot(after_finalize);
    Require(after_finalize.counters.values[0][0][0][0] ==
                    first_snapshot.counters.values[0][0][0][0] &&
                after_finalize.counters.values[0][0][0][1] ==
                    first_snapshot.counters.values[0][0][0][1],
            "no post-finalize increments are accepted");
    const std::string first_json = ReadFile(first / "coverage.json");

    const auto second = MakeDirectory("deterministic-a");
    Observer   second_observer;
    StartFresh(second_observer, second);
    for (int call = 0; call < kThreads * kCalls; ++call)
    {
        second_observer.RecordSiteFixedValueForTest(
            static_cast<std::uint32_t>(call & 1), 4);
    }
    Require(second_observer.Finalize() == FinalizeStatus::Accepted,
            "finalize deterministic run");
    const std::string second_json = ReadFile(second / "coverage.json");
    Require(second_json.find("\"observer_byte_budget\":") !=
                std::string::npos,
            "coverage publishes the fixed observer byte budget");
    Require(second_json.find(
                "\"operation_metadata\":{\"operation_id\":\"NRD-OP-0002\""
                ",\"runtime_join_key\":\"d3d:0x826A3568\""
                ",\"roles\":[\"wrapper\",\"lowering-boundary\"]"
                ",\"contract_ids\":[\"NRD-CONTRACT-0001\"]") !=
                    std::string::npos &&
                second_json.find(
                    "\"hook_sites\":[{\"address\":2184199740,"
                    "\"phase\":\"value\","
                    "\"discriminator\":\"primitive-4\"},"
                    "{\"address\":2184199820,\"phase\":\"value\","
                    "\"discriminator\":\"primitive-4\"}]") !=
                    std::string::npos &&
                second_json.find(
                    "\"registers\":[],\"value_domains\":["
                    "{\"id\":\"primitive-4\",\"value\":4,"
                    "\"selection\":\"site-fixed\"},"
                    "{\"id\":\"unknown\",\"value\":null,"
                    "\"selection\":\"unmapped-input\"}]") !=
                    std::string::npos,
            "coverage publishes accepted operation discriminator metadata");
    Require(second_json.find("\"transition_attribution_valid\":true") !=
                std::string::npos,
            "ordered checkpoints preserve transition attribution");
    Require(CountOccurrences(second_json, "    {\"segment\":") ==
                kSegmentCount * kOperationCount * kValueDomainCount * kHookSiteCount,
            "coverage publishes every segmented counter row");
    const std::uint64_t expected_site_count =
        static_cast<std::uint64_t>(kThreads * kCalls / 2);
    const std::uint32_t site_addresses[kHookSiteCount] = {
        0x82303E3Cu, 0x82303E8Cu
    };
    const char* domain_ids[kValueDomainCount] = { "primitive-4", "unknown" };
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
        {
            for (std::size_t site = 0; site < kHookSiteCount; ++site)
            {
                const std::uint64_t expected_count = segment == 0 && domain == 0
                                                         ? expected_site_count
                                                         : 0;
                const std::string   row =
                    "    {\"segment\":" + std::to_string(segment) +
                    ",\"operation\":0,\"operation_id\":\"NRD-OP-0002\""
                    ",\"runtime_join_key\":\"d3d:0x826A3568\""
                    ",\"contract_id\":\"NRD-CONTRACT-0001\",\"domain\":" +
                    std::to_string(domain) + ",\"domain_id\":\"" +
                    domain_ids[domain] + "\",\"site\":" +
                    std::to_string(site) + ",\"site_address\":" +
                    std::to_string(site_addresses[site]) + ",\"count\":" +
                    std::to_string(expected_count) + "}";
                Require(second_json.find(row) != std::string::npos,
                        "counter row preserves generated semantic metadata");
            }
        }
    }

    const auto third = MakeDirectory("deterministic-b");
    Observer   third_observer;
    StartFresh(third_observer, third);
    for (int call = 0; call < kThreads * kCalls; ++call)
    {
        third_observer.RecordSiteFixedValueForTest(
            static_cast<std::uint32_t>(call & 1), 4);
    }
    Require(third_observer.Finalize() == FinalizeStatus::Accepted,
            "finalize second deterministic run");
    const std::string third_json = ReadFile(third / "coverage.json");
    Require(second_json == third_json, "deterministic JSON output");
    Require(first_json.find("\"rejected_in_flight\":") != std::string::npos,
            "concurrent output records admission counter");
}

void TestExitClassesAndRecovery()
{
    const auto close = MakeDirectory("window-close");
    Observer   close_observer;
    StartFresh(close_observer, close);
    Require(close_observer.Finalize(ExitClass::WindowClose) ==
                FinalizeStatus::Accepted,
            "window close finalization");
    const std::string close_json = ReadFile(close / "coverage.json");
    Require(close_json.find("\"exit_class\":\"window_close\"") != std::string::npos &&
                close_json.find("\"lifetime_evaluation\":\"not-evaluated\"") !=
                    std::string::npos &&
                close_json.find("\"complete\":true") != std::string::npos &&
                close_json.find("\"incomplete\":false") != std::string::npos,
            "window close preserves drained coverage");
    Require(!std::filesystem::exists(close / "coverage.incomplete"),
            "successful window close removes incomplete sentinel");

    const auto recovery = MakeDirectory("recovery");
    {
        std::ofstream stale(recovery / "coverage.json.tmp", std::ios::binary);
        stale << "stale";
    }
    Observer recovery_observer;
    StartFresh(recovery_observer, recovery);
    Require(std::filesystem::exists(recovery / "coverage.incomplete"),
            "start writes exact incomplete sentinel");
    Require(recovery_observer.Finalize(ExitClass::Shutdown) ==
                FinalizeStatus::Accepted,
            "shutdown finalization");
    const std::string recovery_json = ReadFile(recovery / "coverage.json");
    Require(recovery_json.find("\"recovered_incomplete\":true") !=
                    std::string::npos &&
                recovery_json.find("\"exit_class\":\"shutdown\"") !=
                    std::string::npos &&
                recovery_json.find("\"lifetime_evaluation\":\"not-evaluated\"") !=
                    std::string::npos &&
                recovery_json.find("\"complete\":true") != std::string::npos &&
                recovery_json.find("\"incomplete\":false") != std::string::npos,
            "incomplete recovery is recorded");
    Require(!std::filesystem::exists(recovery / "coverage.incomplete"),
            "successful shutdown removes incomplete sentinel");
}

void TestFinalizationDrainTimeout()
{
    const auto directory = MakeDirectory("drain-timeout");
    Observer   observer;
    StartFresh(observer, directory);
    observer.SetInFlightForTest(1);
    Require(observer.Finalize() == FinalizeStatus::Incomplete,
            "finalization reports bounded drain timeout");
    ObserverSnapshot snapshot;
    observer.Snapshot(snapshot);
    Require(snapshot.incomplete && !snapshot.finalized &&
                snapshot.anomalies[static_cast<std::size_t>(
                                       AnomalyId::FinalizationDrainTimeout)]
                        .count >= 1,
            "drain timeout anomaly is recorded before incomplete return");
    observer.SetInFlightForTest(0);
    Require(observer.Finalize() == FinalizeStatus::Incomplete,
            "finalization timeout owns the terminal incomplete outcome");
    Require(std::filesystem::exists(directory / "coverage.incomplete") &&
                !std::filesystem::exists(directory / "coverage.json"),
            "incomplete timeout leaves sentinel without accepted artifact");
}

} // namespace

int main()
{
    TestDisabledAndHooks();
    TestValidationAndCheckpoints();
    TestConcurrencyAndDeterminism();
    TestExitClassesAndRecovery();
    TestFinalizationDrainTimeout();
    std::cout << "native_renderer_coverage_test: PASS\n";
    return 0;
}
