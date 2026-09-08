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

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path makeDirectory(const char* suffix)
{
    const auto      path = std::filesystem::temp_directory_path() /
                           (std::string("rerevved-b1b-") + suffix);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    require(!error, "create temporary output directory");
    return path;
}

StartOptions options(const std::filesystem::path& directory)
{
    static std::string output;
    output = directory.string();
    StartOptions options;
    options.runId           = "NRD-RUN-20260829-0001";
    options.transitionId    = "NRD-TRANS-0001";
    options.inputDigest     = kAcceptedInputDigest;
    options.outputDirectory = output.c_str();
    options.xenosEnabled    = true;
    options.rovEnabled      = true;
    return options;
}

SnapshotFields fields()
{
    SnapshotFields fields;
    fields.frameSequence   = 71;
    fields.validFields     = 0x1f;
    fields.gameplayActive  = true;
    fields.interfaceUpdate = true;
    fields.activePlayer    = -1;
    fields.humanPlayerMask = 5;
    fields.turnOwnerKnown  = true;
    fields.humanTurn       = false;
    fields.available       = true;
    fields.civilization    = 2;
    fields.era             = 3;
    fields.year            = 1492;
    fields.turn            = 10;
    return fields;
}

bool sameFields(const SnapshotFields& left, const SnapshotFields& right)
{
    return left.frameSequence == right.frameSequence &&
           left.validFields == right.validFields &&
           left.gameplayActive == right.gameplayActive &&
           left.interfaceUpdate == right.interfaceUpdate &&
           left.activePlayer == right.activePlayer &&
           left.humanPlayerMask == right.humanPlayerMask &&
           left.turnOwnerKnown == right.turnOwnerKnown &&
           left.humanTurn == right.humanTurn &&
           left.available == right.available &&
           left.civilization == right.civilization && left.era == right.era &&
           left.year == right.year && left.turn == right.turn;
}

void startFresh(Observer& observer, const std::filesystem::path& directory)
{
    const StartStatus status = observer.Start(options(directory));
    require(status == StartStatus::Accepted, "start observer");
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "open generated coverage");
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::size_t countOccurrences(const std::string& text, const std::string& needle)
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
    require(Observer::Instance().ObjectSizeBytes() == kObserverByteBudget,
            "observer object matches fixed byte budget");
    ObserverSnapshot snapshot;
    Snapshot(snapshot);
    require(!snapshot.enabled, "observer starts disabled");
    RecordSiteFixedValue(0, 4);
    require(!Observer::Instance().SetCounterForTest(0, 0, 0, 0, 1),
            "disabled counter test hook is ignored");
    Snapshot(snapshot);
    require(snapshot.counters.values[0][0][1][0] == 0,
            "disabled recorder does not count");

    const auto directory = makeDirectory("disabled");
    startFresh(Observer::Instance(), directory);
    RecordSiteFixedValue(0, 4);
    RecordSiteFixedValue(1, 0);
    RecordSiteFixedValue(1, 5);
    Snapshot(snapshot);
    require(snapshot.counters.values[0][0][0][0] == 1,
            "primitive-4 value site zero count");
    require(snapshot.counters.values[0][0][1][1] == 2,
            "unknown value domain site one count");
    require(snapshot.counters.values[0][0][1][0] == 0,
            "primitive-4 domain excludes unknown values");
    require(snapshot.counters.values[0][0][0][1] == 0,
            "unknown domain excludes primitive-4");
    require(Observer::Instance().SetCounterForTest(
                0, 0, 0, 0, UINT64_MAX - 1u),
            "set saturation test counter");
    std::vector<std::thread> saturationWorkers;
    for (int worker = 0; worker < 8; ++worker)
    {
        saturationWorkers.emplace_back([]
                                       {
                                           RecordSiteFixedValue(0, 4);
                                       });
    }
    for (auto& worker : saturationWorkers)
    {
        worker.join();
    }
    Snapshot(snapshot);
    require(snapshot.counters.values[0][0][0][0] == UINT64_MAX,
            "counter saturates");
    require(snapshot.counters.saturatedFailures >= 1,
            "counter saturation failure is counted");
    require(snapshot.anomalies[static_cast<std::size_t>(AnomalyId::CounterSaturated)].count >= 1,
            "counter saturation anomaly ID is recorded");
    require(Finalize() == FinalizeStatus::Accepted, "finalize guest complete");
    RecordSiteFixedValue(0, 4);
    Snapshot(snapshot);
    require(snapshot.counters.values[0][0][0][0] == UINT64_MAX,
            "finalized recorder is disabled");
    require(Finalize() == FinalizeStatus::AlreadyFinalized,
            "finalize is idempotent");
    require(Start(options(directory)) == StartStatus::AlreadyFinalized,
            "a finalized process cannot restart another run");
}

void TestValidationAndCheckpoints()
{
    const auto   directory = makeDirectory("validation");
    Observer     invalid;
    StartOptions startOptions = options(directory);
    startOptions.runId        = "NRD-RUN-2026X829-0001";
    require(invalid.Start(startOptions) == StartStatus::InvalidRunId,
            "run id range format");
    startOptions              = options(directory);
    startOptions.transitionId = "NRD-TRANS-0012";
    require(invalid.Start(startOptions) == StartStatus::InvalidTransitionId,
            "transition id range format");
    startOptions             = options(directory);
    startOptions.inputDigest = "wrong";
    require(invalid.Start(startOptions) == StartStatus::InvalidInputDigest,
            "digest validation");
    startOptions              = options(directory);
    startOptions.xenosEnabled = false;
    require(invalid.Start(startOptions) == StartStatus::InvalidXenos,
            "xenos validation");
    startOptions            = options(directory);
    startOptions.rovEnabled = false;
    require(invalid.Start(startOptions) == StartStatus::InvalidRov, "rov validation");

    const auto root  = makeDirectory("contained-root");
    const auto child = root / "capture";
    std::filesystem::create_directories(child);
    std::string rootText = root.string();
    Observer    contained;
    startOptions            = options(child);
    startOptions.outputRoot = rootText.c_str();
    require(contained.Start(startOptions) == StartStatus::Accepted,
            "contained output directory accepted");
    require(contained.Finalize() == FinalizeStatus::Accepted,
            "finalize contained output directory");
    const auto outside = makeDirectory("outside-root");
    Observer   outsideObserver;
    startOptions            = options(outside);
    startOptions.outputRoot = rootText.c_str();
    require(outsideObserver.Start(startOptions) == StartStatus::InvalidOutputRoot,
            "output directory escape rejected");

    Observer checkpointObserver;
    startFresh(checkpointObserver, directory);

    const SnapshotFields snapshotFields = fields();
    require(checkpointObserver.RecordSegment(0, snapshotFields) ==
                CheckpointStatus::Accepted,
            "first segment is accepted");
    checkpointObserver.RecordSiteFixedValueForTest(0, 4);
    require(checkpointObserver.RecordCheckpoint(2, 1, snapshotFields) ==
                CheckpointStatus::InvalidMark,
            "out-of-order checkpoint is rejected");
    require(checkpointObserver.RecordCheckpoint(2, 0, snapshotFields) ==
                CheckpointStatus::InvalidMark,
            "checkpoint segment must equal mark plus one");
    ObserverSnapshot rejectedSnapshot;
    checkpointObserver.Snapshot(rejectedSnapshot);
    require(!rejectedSnapshot.segments[1].accepted &&
                !rejectedSnapshot.segments[2].accepted &&
                !rejectedSnapshot.checkpoints[0].accepted &&
                !rejectedSnapshot.checkpoints[1].accepted,
            "rejected checkpoints publish neither checkpoint nor segment");
    require(checkpointObserver.RecordCheckpoint(1, 0, snapshotFields) ==
                CheckpointStatus::Accepted,
            "first checkpoint advances to its segment");
    SnapshotFields retryFields = snapshotFields;
    retryFields.frameSequence  = snapshotFields.frameSequence + 1u;
    require(checkpointObserver.RecordCheckpoint(1, 0, retryFields) ==
                CheckpointStatus::AlreadyRecorded,
            "accepted checkpoint retry is rejected");
    ObserverSnapshot retrySnapshot;
    checkpointObserver.Snapshot(retrySnapshot);
    require(sameFields(retrySnapshot.checkpoints[0].fields, snapshotFields) &&
                sameFields(retrySnapshot.segments[1].fields, snapshotFields),
            "checkpoint retry cannot mutate either accepted row");
    checkpointObserver.RecordSiteFixedValueForTest(1, 4);
    for (std::uint32_t mark = 1; mark < kCheckpointCapacity; ++mark)
    {
        const std::uint32_t segment = mark + 1u;
        require(checkpointObserver.RecordCheckpoint(segment, mark, snapshotFields) ==
                    CheckpointStatus::Accepted,
                "manual checkpoints are monotonic");
    }
    require(checkpointObserver.RecordSegment(7, snapshotFields) ==
                CheckpointStatus::Accepted,
            "final segment is accepted");
    checkpointObserver.RecordSiteFixedValueForTest(0, 4);
    require(checkpointObserver.RecordSegment(8, snapshotFields) ==
                CheckpointStatus::InvalidSegment,
            "ninth segment is rejected");
    require(checkpointObserver.RecordCheckpoint(0, 6, snapshotFields) ==
                CheckpointStatus::InvalidMark,
            "seventh checkpoint is rejected");
    ObserverSnapshot snapshot;
    checkpointObserver.Snapshot(snapshot);
    require(!snapshot.transitionAttributionValid &&
                snapshot.anomalies[static_cast<std::size_t>(
                                       AnomalyId::CheckpointSequence)]
                        .count == 4,
            "checkpoint mismatches and retry have stable sequence anomalies");
    require(snapshot.segments[7].accepted &&
                snapshot.segments[7].fields.frameSequence == snapshotFields.frameSequence,
            "segment snapshot preserves accepted gameplay fields");
    require(snapshot.counters.values[0][0][0][0] == 1 &&
                snapshot.counters.values[1][0][0][1] == 1 &&
                snapshot.counters.values[7][0][0][0] == 1,
            "counter rows stay separated by accepted segment");
    for (std::size_t mark = 0; mark < kCheckpointCapacity; ++mark)
    {
        const std::size_t         segmentIndex = mark + 1u;
        const CheckpointSnapshot& saved        = snapshot.checkpoints[mark];
        const SegmentSnapshot&    segment      = snapshot.segments[segmentIndex];
        require(saved.accepted && saved.segment == segmentIndex &&
                    segment.accepted && sameFields(saved.fields, segment.fields) &&
                    sameFields(saved.fields, snapshotFields),
                "checkpoint publishes an identical accepted segment snapshot");
    }
    require(checkpointObserver.Finalize() == FinalizeStatus::Accepted,
            "finalize checkpoint run");
    require(checkpointObserver.RecordSegment(0, snapshotFields) == CheckpointStatus::Disabled,
            "segment recording is disabled after finalization");
    require(checkpointObserver.RecordCheckpoint(0, 0, snapshotFields) ==
                CheckpointStatus::Disabled,
            "checkpoint recording is disabled after finalization");
}

void TestConcurrencyAndDeterminism()
{
    constexpr int kThreads = 8;
    const auto    first    = makeDirectory("concurrency-a");
    Observer      firstObserver;
    startFresh(firstObserver, first);
    constexpr int            kCalls = 2000;
    std::atomic<int>         ready{ 0 };
    std::atomic<bool>        go{ false };
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        workers.emplace_back([&firstObserver, &ready, &go]
                             {
                                 ready.fetch_add(1, std::memory_order_release);
                                 while (!go.load(std::memory_order_acquire))
                                 {
                                     std::this_thread::yield();
                                 }
                                 for (int call = 0; call < kCalls; ++call)
                                 {
                                     firstObserver.RecordSiteFixedValueForTest(
                                         static_cast<std::uint32_t>(call & 1), 4);
                                 }
                             });
    }
    while (ready.load(std::memory_order_acquire) != kThreads)
    {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    require(firstObserver.Finalize() == FinalizeStatus::Accepted,
            "finalization overlaps concurrent admission");
    for (auto& worker : workers)
    {
        worker.join();
    }
    ObserverSnapshot firstSnapshot;
    firstObserver.Snapshot(firstSnapshot);
    const std::uint64_t firstCount =
        firstSnapshot.counters.values[0][0][0][0] +
        firstSnapshot.counters.values[0][0][0][1];
    require(firstCount <= static_cast<std::uint64_t>(kThreads * kCalls),
            "concurrent admission remains bounded");
    for (int call = 0; call < 100; ++call)
    {
        firstObserver.RecordSiteFixedValueForTest(0, 4);
    }
    ObserverSnapshot afterFinalize;
    firstObserver.Snapshot(afterFinalize);
    require(afterFinalize.counters.values[0][0][0][0] ==
                    firstSnapshot.counters.values[0][0][0][0] &&
                afterFinalize.counters.values[0][0][0][1] ==
                    firstSnapshot.counters.values[0][0][0][1],
            "no post-finalize increments are accepted");
    const std::string firstJson = readFile(first / "coverage.json");

    const auto second = makeDirectory("deterministic-a");
    Observer   secondObserver;
    startFresh(secondObserver, second);
    for (int call = 0; call < kThreads * kCalls; ++call)
    {
        secondObserver.RecordSiteFixedValueForTest(
            static_cast<std::uint32_t>(call & 1), 4);
    }
    require(secondObserver.Finalize() == FinalizeStatus::Accepted,
            "finalize deterministic run");
    const std::string secondJson = readFile(second / "coverage.json");
    require(secondJson.find("\"observer_byte_budget\":") !=
                std::string::npos,
            "coverage publishes the fixed observer byte budget");
    require(secondJson.find(
                "\"operation_metadata\":{\"operation_id\":\"NRD-OP-0002\""
                ",\"runtime_join_key\":\"d3d:0x826A3568\""
                ",\"roles\":[\"wrapper\",\"lowering-boundary\"]"
                ",\"contract_ids\":[\"NRD-CONTRACT-0001\"]") !=
                    std::string::npos &&
                secondJson.find(
                    "\"hook_sites\":[{\"address\":2184199740,"
                    "\"phase\":\"value\","
                    "\"discriminator\":\"primitive-4\"},"
                    "{\"address\":2184199820,\"phase\":\"value\","
                    "\"discriminator\":\"primitive-4\"}]") !=
                    std::string::npos &&
                secondJson.find(
                    "\"registers\":[],\"value_domains\":["
                    "{\"id\":\"primitive-4\",\"value\":4,"
                    "\"selection\":\"site-fixed\"},"
                    "{\"id\":\"unknown\",\"value\":null,"
                    "\"selection\":\"unmapped-input\"}]") !=
                    std::string::npos,
            "coverage publishes accepted operation discriminator metadata");
    require(secondJson.find("\"transition_attribution_valid\":true") !=
                std::string::npos,
            "ordered checkpoints preserve transition attribution");
    require(countOccurrences(secondJson, "    {\"segment\":") ==
                kSegmentCount * kOperationCount * kValueDomainCount * kHookSiteCount,
            "coverage publishes every segmented counter row");
    const std::uint64_t expectedSiteCount =
        static_cast<std::uint64_t>(kThreads * kCalls / 2);
    const std::uint32_t siteAddresses[kHookSiteCount] = {
        0x82303E3Cu, 0x82303E8Cu
    };
    const char* domainIds[kValueDomainCount] = { "primitive-4", "unknown" };
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment)
    {
        for (std::size_t domain = 0; domain < kValueDomainCount; ++domain)
        {
            for (std::size_t site = 0; site < kHookSiteCount; ++site)
            {
                const std::uint64_t expectedCount = segment == 0 && domain == 0
                                                        ? expectedSiteCount
                                                        : 0;
                const std::string   row =
                    "    {\"segment\":" + std::to_string(segment) +
                    ",\"operation\":0,\"operation_id\":\"NRD-OP-0002\""
                    ",\"runtime_join_key\":\"d3d:0x826A3568\""
                    ",\"contract_id\":\"NRD-CONTRACT-0001\",\"domain\":" +
                    std::to_string(domain) + ",\"domain_id\":\"" +
                    domainIds[domain] + "\",\"site\":" +
                    std::to_string(site) + ",\"site_address\":" +
                    std::to_string(siteAddresses[site]) + ",\"count\":" +
                    std::to_string(expectedCount) + "}";
                require(secondJson.find(row) != std::string::npos,
                        "counter row preserves generated semantic metadata");
            }
        }
    }

    const auto third = makeDirectory("deterministic-b");
    Observer   thirdObserver;
    startFresh(thirdObserver, third);
    for (int call = 0; call < kThreads * kCalls; ++call)
    {
        thirdObserver.RecordSiteFixedValueForTest(
            static_cast<std::uint32_t>(call & 1), 4);
    }
    require(thirdObserver.Finalize() == FinalizeStatus::Accepted,
            "finalize second deterministic run");
    const std::string thirdJson = readFile(third / "coverage.json");
    require(secondJson == thirdJson, "deterministic JSON output");
    require(firstJson.find("\"rejected_in_flight\":") != std::string::npos,
            "concurrent output records admission counter");
}

void TestExitClassesAndRecovery()
{
    const auto close = makeDirectory("window-close");
    Observer   closeObserver;
    startFresh(closeObserver, close);
    require(closeObserver.Finalize(ExitClass::WindowClose) ==
                FinalizeStatus::Accepted,
            "window close finalization");
    const std::string closeJson = readFile(close / "coverage.json");
    require(closeJson.find("\"exit_class\":\"window_close\"") != std::string::npos &&
                closeJson.find("\"lifetime_evaluation\":\"not-evaluated\"") !=
                    std::string::npos &&
                closeJson.find("\"complete\":true") != std::string::npos &&
                closeJson.find("\"incomplete\":false") != std::string::npos,
            "window close preserves drained coverage");
    require(!std::filesystem::exists(close / "coverage.incomplete"),
            "successful window close removes incomplete sentinel");

    const auto recovery = makeDirectory("recovery");
    {
        std::ofstream stale(recovery / "coverage.json.tmp", std::ios::binary);
        stale << "stale";
    }
    Observer recoveryObserver;
    startFresh(recoveryObserver, recovery);
    require(std::filesystem::exists(recovery / "coverage.incomplete"),
            "start writes exact incomplete sentinel");
    require(recoveryObserver.Finalize(ExitClass::Shutdown) ==
                FinalizeStatus::Accepted,
            "shutdown finalization");
    const std::string recoveryJson = readFile(recovery / "coverage.json");
    require(recoveryJson.find("\"recovered_incomplete\":true") !=
                    std::string::npos &&
                recoveryJson.find("\"exit_class\":\"shutdown\"") !=
                    std::string::npos &&
                recoveryJson.find("\"lifetime_evaluation\":\"not-evaluated\"") !=
                    std::string::npos &&
                recoveryJson.find("\"complete\":true") != std::string::npos &&
                recoveryJson.find("\"incomplete\":false") != std::string::npos,
            "incomplete recovery is recorded");
    require(!std::filesystem::exists(recovery / "coverage.incomplete"),
            "successful shutdown removes incomplete sentinel");
}

void TestFinalizationDrainTimeout()
{
    const auto directory = makeDirectory("drain-timeout");
    Observer   observer;
    startFresh(observer, directory);
    observer.SetInFlightForTest(1);
    require(observer.Finalize() == FinalizeStatus::Incomplete,
            "finalization reports bounded drain timeout");
    ObserverSnapshot snapshot;
    observer.Snapshot(snapshot);
    require(snapshot.incomplete && !snapshot.finalized &&
                snapshot.anomalies[static_cast<std::size_t>(
                                       AnomalyId::FinalizationDrainTimeout)]
                        .count >= 1,
            "drain timeout anomaly is recorded before incomplete return");
    observer.SetInFlightForTest(0);
    require(observer.Finalize() == FinalizeStatus::Incomplete,
            "finalization timeout owns the terminal incomplete outcome");
    require(std::filesystem::exists(directory / "coverage.incomplete") &&
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
