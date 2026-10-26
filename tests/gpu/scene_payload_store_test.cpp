#include "directory_removal.hpp"
#include "gpu/diagnostics/scene_payload_store.h"
#include "gpu/diagnostics/scene_target_readback.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <toml++/toml.hpp>

using namespace rerevved::gpu::diagnostics;

namespace
{

ScenePayloadBinding binding(std::uint64_t frame, std::uint64_t event, std::uint64_t generation)
{
    ScenePayloadBinding b;
    b.frame      = frame;
    b.event      = event;
    b.submission = 10;
    b.parent     = 9;
    b.generation = generation;
    b.version    = event;
    b.resource   = "texture:8192";
    b.role       = "sampled_base";
    b.address    = 4096;
    b.size       = 3;
    b.physical   = true;
    return b;
}

} // namespace

TEST_CASE("scene_payload_store", "[ctest][scene-payload-store]")
{
    SceneAllocationTracker allocations(2);
    const auto             generation1 = allocations.Allocate({ 4096, 4096 });
    UNSCOPED_INFO("allocation subrange identity");
    REQUIRE((generation1 && allocations.Find({ 4100, 4 }) == generation1));
    UNSCOPED_INFO("cross-allocation range cannot inherit lifetime");
    REQUIRE(!allocations.Find({ 8190, 4 }));
    UNSCOPED_INFO("free invalidates generation");
    REQUIRE((allocations.Free(4096) == generation1 && !allocations.Find({ 4100, 4 })));
    const auto generation2 = allocations.Allocate({ 4096, 4096 });
    UNSCOPED_INFO("same address reuse needs a new generation");
    REQUIRE(generation2 > generation1);
    UNSCOPED_INFO("allocation live budget");
    REQUIRE((allocations.Allocate({ 8192, 4096 }) && !allocations.Allocate({ 12288, 4096 }) && allocations.Overflow() == 1));
    UNSCOPED_INFO("overflow invalidates every live generation");
    REQUIRE((!allocations.Find({ 4100, 4 }) && !allocations.Find({ 8192, 4 })));
    UNSCOPED_INFO("overflow cannot resume incomplete lifetime tracking");
    REQUIRE(!allocations.Allocate({ 16384, 4096 }));
    SceneAllocationTracker recordBudget;
    recordBudget.Allocate({ 4096, 4096 });
    recordBudget.Invalidate();
    UNSCOPED_INFO("record budget invalidates live map");
    REQUIRE((recordBudget.Overflow() == 1 && !recordBudget.Find({ 4096, 4 })));
    SceneAllocationTracker overlap(2);
    const auto             old         = overlap.Allocate({ 100, 20 });
    const auto             neighbor    = overlap.Allocate({ 130, 20 });
    const auto             replacement = overlap.Allocate({ 110, 20 });
    UNSCOPED_INFO("overlap ends the entire old generation and replaces it even at capacity");
    REQUIRE((replacement > old && !overlap.Find({ 100, 4 }) && overlap.Find({ 110, 20 }) == replacement));
    UNSCOPED_INFO("adjacent allocation survives and overlap is counted separately from overflow");
    REQUIRE((overlap.Find({ 130, 20 }) == neighbor && overlap.Overlaps() == 1 && !overlap.Overflow()));
    const auto spanning = overlap.Allocate({ 115, 30 });
    UNSCOPED_INFO("one new range can implicitly end multiple live generations");
    REQUIRE((spanning > replacement && !overlap.Find({ 110, 4 }) && !overlap.Find({ 145, 4 }) && overlap.Overlaps() == 3));
    UNSCOPED_INFO("tracking continues after overlap");
    REQUIRE((!overlap.Free(110) && overlap.Allocate({ 200, 20 }) > spanning));
    UNSCOPED_INFO("a later budget overflow still invalidates tracking");
    REQUIRE((!overlap.Allocate({ 300, 20 }) && overlap.Overflow() == 1 && !overlap.Find({ 115, 30 })));
    const std::array<std::uint8_t, 3> abc{ 'a', 'b', 'c' }, changed{ 'a', 'b', 'd' };
    unsigned                          reads  = 0;
    const auto                        reader = [&]() -> std::span<const std::uint8_t>
    {
        ++reads;
        return abc;
    };
    ScenePayloadStore disabled(false, 0, "disabled");
    UNSCOPED_INFO("disabled store read guest memory");
    REQUIRE((disabled.Capture(binding(1, 1, 1), reader).sha256.empty() && !reads));
    UNSCOPED_INFO("disabled store accounting");
    REQUIRE((!disabled.Active() && !disabled.Bytes() && !disabled.Overflow()));

    ScenePayloadStore store(true, 3, "fixture-capture");
    const auto        initial = store.Capture(binding(4, 2, 7), reader, false);
    UNSCOPED_INFO("SHA-256 identity");
    REQUIRE(initial.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto repeated = store.Capture(binding(5, 11, 8), reader);
    UNSCOPED_INFO("dedup across resource generations");
    REQUIRE((repeated.sha256 == initial.sha256 && store.Bytes() == 3));
    auto history       = initial;
    history.submission = 10;
    store.Publish(history);
    const auto overflow = store.Capture(binding(5, 12, 8), [&]() -> std::span<const std::uint8_t>
                                        {
                                            return changed;
                                        });
    UNSCOPED_INFO("budget must reject entire payload");
    REQUIRE((overflow.error == "payload_byte_budget" && overflow.sha256.empty()));
    UNSCOPED_INFO("hard budget accounting");
    REQUIRE((store.Bytes() == 3 && store.Overflow() == 1 && !store.Active()));
    const auto previousReads = reads;
    UNSCOPED_INFO("stopped retention read memory");
    REQUIRE((store.Capture(binding(5, 13, 8), reader).error == "payload_retention_stopped" && reads == previousReads));

    const auto directory = std::filesystem::temp_directory_path() /
                           ("rerevved-scene-payload-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UNSCOPED_INFO("test directory");
    REQUIRE(std::filesystem::create_directory(directory));

    const rerevved::tests::ScopedDirectoryRemoval removeDirectory(directory);

    SceneTargetRecord target;
    target.kind           = "seed";
    target.resource       = "attachment:1";
    target.aspect         = "depth";
    target.format         = "D24_UNORM";
    target.sampleLayout   = "pixel-major sample-0";
    target.resolve        = "none";
    target.frame          = 4;
    target.generation     = 2;
    target.completion     = 7;
    target.rectangle      = { 0, 0, 1, 1 };
    target.rowBytes       = 3;
    target.rows           = 1;
    target.samples        = 1;
    target.bytesPerSample = 3;
    SceneTargetReadback targets("fixture-capture", 6, 3);
    UNSCOPED_INFO("seed receipt");
    REQUIRE(targets.Capture(target, abc));
    target.kind    = "reference";
    target.aspect  = "color";
    target.format  = "RGB8";
    target.resolve = "resolved swap output";
    UNSCOPED_INFO("lossless reference receipt");
    REQUIRE(targets.Capture(target, abc));
    UNSCOPED_INFO("target byte budget");
    REQUIRE((!targets.Capture(target, abc) && targets.Stats().overflow == 1));
    UNSCOPED_INFO("target record budget");
    REQUIRE((!targets.Capture(target, abc) && targets.Stats().overflow == 2));
    targets.Flush(directory);
    const auto targetManifest = toml::parse_file((directory / "target-manifest.toml").string());
    UNSCOPED_INFO("target record serialization");
    REQUIRE((targetManifest["records"].as_array()->size() == 3 && targets.Stats().bytes == 6));
    SceneTargetReadback invalidTargets("invalid", 20);
    target.generation = 0;
    UNSCOPED_INFO("unknown target generation refused");
    REQUIRE((!invalidTargets.Capture(target, abc) && invalidTargets.Stats().failures == 1));
    invalidTargets.Gap(target, "sdk_readback_unavailable");
    UNSCOPED_INFO("missing readback stays a gap");
    REQUIRE(invalidTargets.Stats().gaps == 1);
    store.Flush(directory);
    const auto manifest = toml::parse_file((directory / "payload-manifest.toml").string());
    UNSCOPED_INFO("manifest capture binding");
    REQUIRE(manifest["capture_id"].value_or("") == std::string_view("fixture-capture"));
    const auto* rows = manifest["bindings"].as_array();
    UNSCOPED_INFO("one binding per publication including failures");
    REQUIRE((rows && rows->size() == 4));
    const auto& row = *(*rows)[0].as_table();
    UNSCOPED_INFO("manifest lost call/resource identity");
    REQUIRE((row["capture_id"].value_or("") == std::string_view("fixture-capture") && row["frame"].value_or(0) == 5 &&
             row["event"].value_or(0) == 11 && row["submission"].value_or(0) == 10 && row["parent_call"].value_or(0) == 9 &&
             row["generation"].value_or(0) == 8 && row["version"].value_or(0) == 11 && row["address"].value_or(0) == 4096 &&
             row["physical"].value_or(false)));
    const auto& historyRow = *(*rows)[1].as_table();
    UNSCOPED_INFO("history origin was overwritten");
    REQUIRE((historyRow["frame"].value_or(0) == 4 && historyRow["generation"].value_or(0) == 7));
    unsigned files = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory / "payloads"))
    {
        ++files;
        UNSCOPED_INFO("blob silently truncated");
        REQUIRE(file.file_size() == 3);
    }
    UNSCOPED_INFO("dedup wrote multiple files");
    REQUIRE(files == 1);
    store.Flush(directory);
    UNSCOPED_INFO("flush repeated bindings");
    REQUIRE(toml::parse_file((directory / "payload-manifest.toml").string())["bindings"].as_array()->size() == 4);

    ScenePayloadStore concurrent(true, 3, "concurrent");
    const auto        concurrentDirectory = directory / "concurrent";
    UNSCOPED_INFO("concurrent test directory");
    REQUIRE(std::filesystem::create_directory(concurrentDirectory));
    std::atomic<bool> done{ false };
    std::jthread      writer([&]
                             {
                            while (!done.load())
                            {
                                concurrent.Flush(concurrentDirectory);
                                std::this_thread::yield();
                            }
                            concurrent.Flush(concurrentDirectory);
                             });
    for (unsigned i = 0; i < 200; ++i)
        concurrent.Capture(binding(1, i + 1, 1), [&]() -> std::span<const std::uint8_t>
                           {
                               return abc;
                           });
    done.store(true);
    writer.join();
    const auto concurrentManifest = toml::parse_file((concurrentDirectory / "payload-manifest.toml").string());
    UNSCOPED_INFO("concurrent flush snapshots lose or duplicate no bindings");
    REQUIRE((concurrentManifest["bindings"].as_array()->size() == 200 && concurrent.Bytes() == 3));

    ScenePayloadStore failed(true, 10, "failure");
    const auto        bad = failed.Capture(binding(1, 1, 1), []() -> std::span<const std::uint8_t>
                                           {
                                        throw std::runtime_error("safe_read_failure");
                                           });
    UNSCOPED_INFO("safe-read failure not recorded");
    REQUIRE((bad.error == "safe_read_failure" && failed.Failures() == 1 && !failed.Bytes()));
    UNSCOPED_INFO("short read accepted");
    REQUIRE(failed.Capture(binding(1, 2, 1), []() -> std::span<const std::uint8_t>
                           {
                               return {};
                           })
                .error == "payload_read_size");

    const auto clipped = ClipScenePayloadRange({ 90, 30 }, { 100, 10 });
    UNSCOPED_INFO("straddling range clip");
    REQUIRE((clipped.address == 100 && clipped.size == 10));
    UNSCOPED_INFO("half-open disjoint range");
    REQUIRE(!ClipScenePayloadRange({ 90, 10 }, { 100, 10 }).size);
    UNSCOPED_INFO("wrapped input range");
    REQUIRE(!ClipScenePayloadRange({ UINT64_MAX - 2, 4 }, { 0, 100 }).size);
    UNSCOPED_INFO("wrapped allocation range");
    REQUIRE(!ClipScenePayloadRange({ 1, 1 }, { UINT64_MAX, 2 }).size);
    UNSCOPED_INFO("empty range");
    REQUIRE(!ClipScenePayloadRange({ 100, 0 }, { 100, 10 }).size);
}
