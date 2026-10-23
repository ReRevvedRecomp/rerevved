#include "solid_adapter.h"

#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/guest_d3d/solid_draw_adapter.h"
#include "gpu/guest_d3d/solid_draw_reference.h"
#include "gpu/guest_d3d/solid_draw_samples.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <rex/crypto/sha256.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace
{

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (size > 16 * 1024 * 1024)
        throw std::runtime_error("adapter_comparison_file_extent");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("adapter_comparison_file_read");
    return bytes;
}

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
        throw std::runtime_error("adapter_comparison_word_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::vector<std::uint8_t> tile(std::span<const std::uint8_t> full, std::uint32_t x)
{
    return rerevved::gpu::guest_d3d::ExtractSolidDrawReferenceTile(full, x);
}

std::size_t differences(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b)
{
    if (a.size() != b.size())
        throw std::runtime_error("adapter_comparison_sample_extent");
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        count += a[i] != b[i];
    return count;
}

} // namespace

bool CompareSolidAdapterCapture(const std::filesystem::path& capture,
                                const std::filesystem::path& shaderDirectory,
                                const std::filesystem::path& output,
                                std::string&                 error,
                                const std::filesystem::path& referenceDirectory,
                                SolidAdapterDestination      destination)
{
    using namespace rerevved::gpu;
    using namespace rerevved::gpu::guest_d3d;
    toml::table report{ { "comparison_schema", 1 }, { "complete", false }, { "original_or_packet_replay", false } };
    const bool  seeded = destination == SolidAdapterDestination::ReferenceBefore;
    report.insert("destination_source", seeded ? "reference_before" : "transparent_black");
    bool reserved = false;
    try
    {
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("adapter_comparison_output_not_fresh");
        reserved                 = true;
        const auto referenceRoot = !referenceDirectory.empty() ? referenceDirectory : std::filesystem::exists(capture / "reference-0001") ? capture / "reference-0001"
                                                                                                                                          : capture / "reference";
        if (std::filesystem::file_size(referenceRoot / "event.toml") > 4096)
            throw std::runtime_error("adapter_comparison_reference_event_extent");
        const auto event = toml::parse_file((referenceRoot / "event.toml").string());
        if (event["reference_schema"].value_or(0) != 1 || !event["complete"].value_or(false) ||
            !event["content_correlated"].value_or(false))
            throw std::runtime_error("adapter_comparison_reference_incomplete");
        // A completed reference record is sufficient even if live acquisition
        // rejected an earlier queued event. Only opt-in destination seeding
        // consumes reference bytes as native input.
        report.insert("reference_directory", referenceRoot.generic_string());
        report.insert("reference_offline", event["offline"].value_or(false));
        report.insert("address_correlated", event["address_correlated"].value_or(false));
        for (const auto key : { "adapter_vertex_base", "adapter_index_base", "reference_vertex_base", "reference_index_base" })
        {
            const auto value = event[key].value<std::uint32_t>();
            if (!value)
                throw std::runtime_error("adapter_comparison_reference_base_field");
            report.insert(key, *value);
        }
        SolidDrawInputs               inputs;
        SolidDrawRecipe               recipe;
        NativeGameplayShaderInterface shader;
        if (!LoadSolidDrawShader(shaderDirectory, shader, error) ||
            !LoadSolidDrawInputs(capture / "adapter", inputs, error) ||
            !BuildSolidDrawRecipe(inputs, shader, recipe, error))
            throw std::runtime_error(error);
        report.insert("shader_catalog_entry", "specialization-0002");
        report.insert("vertex_dxil_sha256", shader.vertexDxilDigest);
        report.insert("pixel_dxil_sha256", shader.pixelDxilDigest);
        NativeDrawReplayRecipe savedNative;
        if (!LoadNativeDrawReplayRecipe(capture / "adapter" / "native-recipe.toml", savedNative, error))
            throw std::runtime_error(error);
        std::shared_ptr<const NativeDrawReplayAdmission> savedAdmission, derivedAdmission;
        if (!AdmitNativeDrawReplayRecipe(savedNative, savedAdmission, error) ||
            !AdmitNativeDrawReplayRecipe(recipe.native, derivedAdmission, error))
            throw std::runtime_error(error);
        const auto sameBytes = [](auto a, auto b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end());
        };
        const auto& n                = recipe.native;
        const bool  nativeReproduced = savedAdmission->PipelineStateKey() == derivedAdmission->PipelineStateKey() &&
                                       sameBytes(savedNative.vertexBytes(), n.vertexBytes()) && savedNative.indices == n.indices &&
                                       sameBytes(savedNative.vertexShaderBytes(), n.vertexShaderBytes()) &&
                                       sameBytes(savedNative.pixelShaderBytes(), n.pixelShaderBytes()) &&
                                       savedNative.vertexConstants == n.vertexConstants && savedNative.pixelConstants == n.pixelConstants &&
                                       savedNative.sharedConstants == n.sharedConstants && savedNative.clearColor == n.clearColor &&
                                       savedNative.width == n.width && savedNative.height == n.height &&
                                       savedNative.vertexStrideBytes == n.vertexStrideBytes && savedNative.indexCount == n.indexCount &&
                                       savedNative.viewport.x == n.viewport.x && savedNative.viewport.y == n.viewport.y &&
                                       savedNative.viewport.width == n.viewport.width && savedNative.viewport.height == n.viewport.height &&
                                       savedNative.viewport.minDepth == n.viewport.minDepth && savedNative.viewport.maxDepth == n.viewport.maxDepth &&
                                       savedNative.scissor.left == n.scissor.left && savedNative.scissor.top == n.scissor.top &&
                                       savedNative.scissor.right == n.scissor.right && savedNative.scissor.bottom == n.scissor.bottom &&
                                       savedNative.initialSample0.empty() && savedNative.initialSample1.empty();
        const auto  registers        = read(referenceRoot / "registers.be.bin");
        const auto  originalState    = read(capture / "original-state.be.bin");
        if (registers.size() != 0x5003 * 4 || originalState.size() != 0x3500)
            throw std::runtime_error("adapter_comparison_state_extent");
        const auto xenosVs       = read(referenceRoot / "vs.be.bin");
        const auto xenosPs       = read(referenceRoot / "ps.be.bin");
        const auto xenosVertices = read(referenceRoot / "vertices.be.bin");
        const auto xenosIndices  = read(referenceRoot / "indices.be.bin");
        const bool geometryEqual = xenosVertices.size() >= inputs.vertices.size() &&
                                   std::equal(inputs.vertices.begin(), inputs.vertices.end(), xenosVertices.begin()) &&
                                   xenosIndices.size() >= inputs.indices.size() &&
                                   std::equal(inputs.indices.begin(), inputs.indices.end(), xenosIndices.begin());
        report.insert("geometry_bytes_equal", geometryEqual);
        const bool vsEqual = recipe.patchedVertexCode == read(capture / "original-vs.be.bin") && recipe.patchedVertexCode == xenosVs;
        const bool psEqual = inputs.pixelCode == read(capture / "original-ps.be.bin") && inputs.pixelCode == xenosPs;
        report.insert("vertex_shader_byte_identity", vsEqual);
        report.insert("pixel_shader_byte_identity", psEqual);
        report.insert("pixel_shader_original_leg_independent", false);
        report.insert("pixel_shader_original_leg_equal", inputs.pixelCode == read(capture / "original-ps.be.bin"));
        report.insert("pixel_shader_xenos_leg_independent", true);
        report.insert("pixel_shader_xenos_leg_equal", inputs.pixelCode == xenosPs);
        report.insert("saved_pre_original_recipe_reproduced", nativeReproduced && recipe.patchedVertexCode == read(capture / "adapter" / "vs-patched.be.bin") && recipe.effectiveState == read(capture / "adapter" / "effective-state.be.bin"));
        toml::array fixed;
        unsigned    mismatches = 0;
        for (const auto mapping : kSolidFixedWords)
        {
            const auto adapter   = word(recipe.effectiveState, mapping.shadow);
            const auto original  = word(originalState, mapping.shadow);
            const auto reference = word(registers, mapping.hardware * 4);
            const bool equal     = adapter == original && adapter == reference;
            mismatches += !equal;
            fixed.push_back(toml::table{ { "shadow_offset", mapping.shadow }, { "register", mapping.hardware }, { "adapter", adapter }, { "original", original }, { "xenos", reference }, { "equal", equal } });
        }
        report.insert("fixed_words", std::move(fixed));
        report.insert("fixed_state_equal", mismatches == 0);
        report.insert("fixed_state_mismatches", mismatches);
        bool constantsEqual = true, viewportEqual = true;
        bool originalConstantsEqual = true;
        for (const auto group : { std::array<std::uint32_t, 3>{ 0x780, 0x4000, 1024 },
                                  std::array<std::uint32_t, 3>{ 0x1780, 0x4400, 896 },
                                  std::array<std::uint32_t, 3>{ 0x2780, 0x4900, 8 } })
            for (unsigned i = 0; i < group[2]; ++i)
                originalConstantsEqual &= word(inputs.state, group[0] + i * 4) == word(originalState, group[0] + i * 4);
        for (const auto group : kSolidReferenceConstants)
            for (unsigned i = 0; i < group[2]; ++i)
                constantsEqual &= word(inputs.state, group[0] + i * 4) == word(registers, (group[1] + i) * 4);
        unsigned unusedVsDifferences = 0;
        for (unsigned i = 16 * 4; i < 256 * 4; ++i)
            unusedVsDifferences += word(inputs.state, 0x780 + i * 4) != word(registers, (0x4000 + i) * 4);
        report.insert("constant_policy", "VS c0-c15, PS c0-c223, all 8 boolean words");
        report.insert("original_all_constants_equal", originalConstantsEqual);
        report.insert("unused_vs_constant_word_differences", unusedVsDifferences);
        // Compare logical viewport scale/offsets before the emitter's tile
        // window offset. The reference's window only selects the sample tile.
        const std::array<float, 6> viewportRegisters{
            n.viewport.width * 0.5F, n.viewport.x + n.viewport.width * 0.5F, -n.viewport.height * 0.5F, n.viewport.y + n.viewport.height * 0.5F, n.viewport.maxDepth - n.viewport.minDepth, n.viewport.minDepth
        };
        toml::array viewportWords;
        for (unsigned i = 0; i < viewportRegisters.size(); ++i)
        {
            const auto adapter = std::bit_cast<std::uint32_t>(viewportRegisters[i]);
            const auto xenos   = word(registers, (0x210F + i) * 4);
            viewportEqual &= adapter == xenos;
            viewportWords.push_back(toml::table{ { "register", 0x210F + i }, { "adapter", adapter }, { "xenos", xenos }, { "equal", adapter == xenos } });
        }
        report.insert("viewport_registers", std::move(viewportWords));
        report.insert("vs_ps_boolean_constants_equal", constantsEqual && originalConstantsEqual);
        report.insert("referenced_constants_xenos_equal", constantsEqual);
        report.insert("viewport_xenos_equal", viewportEqual);
        const auto window = word(registers, 0x2080 * 4);
        if (window != 0 && window != 0x7D80)
            throw std::runtime_error("adapter_comparison_tile_window");
        const auto x = window == 0 ? 0U : 640U;
        if (seeded && (event["window_offset"].value<std::uint32_t>() != window ||
                       event["width"].value_or(0) != 640 || event["height"].value_or(0) != 720 ||
                       event["sample_planes"].value_or(0) != 2))
            throw std::runtime_error("adapter_comparison_reference_tile_metadata");
        report.insert("reference_x", x);
        report.insert("reference_width", 640);
        report.insert("reference_height", 720);
        report.insert("sample_planes", 2);
        const auto before    = read(referenceRoot / "before.samples");
        const auto reference = read(referenceRoot / "after.samples");
        if (seeded && (before.size() != 640 * 720 * 4 * 2 || reference.size() != before.size()))
            throw std::runtime_error("adapter_reference_sample_extent");
        const bool transparent = before.size() == 640 * 720 * 4 * 2 &&
                                 std::all_of(before.begin(), before.end(), [](auto value)
                                             {
                                                 return value == 0;
                                             });
        report.insert("reference_started_transparent_black", transparent);
        report.insert("reference_black_start_gate_passed", transparent);
        report.insert("reference_before_sha256", rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(before.data()), before.size())));
        report.insert("reference_before_non_clear_bytes", static_cast<std::int64_t>(std::count_if(before.begin(), before.end(), [](auto value)
                                                                                                  {
                                                                                                      return value != 0;
                                                                                                  })));
        report.insert("black_start_required", !seeded);
        if (!transparent && !seeded)
            throw std::runtime_error("adapter_reference_pre_draw_tile_not_transparent_black");
        NativeRendererD3D12 renderer;
        bool                destinationRoundTrip = true, omissionPassed = true, unseededLiveEqual = true;
        if (seeded)
        {
            // The unseeded saved recipe still ties the live in-game render to
            // this offline renderer before the destination is replaced.
            auto unseeded             = savedNative;
            unseeded.outputPath       = output / "unseeded-baseline.samples";
            const auto unseededResult = renderer.ReplayOffscreen(unseeded);
            if (!unseededResult.success)
                throw std::runtime_error(unseededResult.error);
            unseededLiveEqual = read(unseeded.outputPath) == read(capture / "native.samples");
            report.insert("unseeded_live_native_replay_equal", unseededLiveEqual);
            SeedSolidDrawReferenceDestination(savedNative, before, x);
            std::vector<std::uint8_t> initial = savedNative.initialSample0;
            initial.insert(initial.end(), savedNative.initialSample1.begin(), savedNative.initialSample1.end());
            const auto otherTile    = tile(initial, 640 - x);
            const bool cpuRoundTrip = tile(initial, x) == before &&
                                      std::all_of(otherTile.begin(), otherTile.end(), [](auto value)
                                                  {
                                                      return value == 0;
                                                  });
            report.insert("seeded_cpu_planes_round_trip_equal", cpuRoundTrip);
            if (!cpuRoundTrip)
                throw std::runtime_error("adapter_destination_cpu_round_trip_failed");
            // An admitted empty scissor suppresses every panel fragment while
            // retaining the existing replay's initial-plane upload/readback.
            auto seedCheck        = savedNative;
            seedCheck.scissor     = {};
            seedCheck.outputPath  = output / "seed-round-trip.samples";
            const auto seedResult = renderer.ReplayOffscreen(seedCheck);
            if (!seedResult.success)
                throw std::runtime_error(seedResult.error);
            destinationRoundTrip = read(seedCheck.outputPath) == initial;
            report.insert("seeded_gpu_planes_round_trip_equal", destinationRoundTrip);
            report.insert("seeded_gpu_round_trip_method", "empty_scissor_no_panel_fragments");
            if (!destinationRoundTrip)
                throw std::runtime_error("adapter_destination_gpu_round_trip_failed");
            // Omission submits no replay: retain the initialized planes alone.
            // The preceding GPU check proves these bytes survive upload/readback.
            std::ofstream omitted(output / "omission.samples", std::ios::binary);
            omitted.write(reinterpret_cast<const char*>(initial.data()), initial.size());
            omitted.close();
            if (!omitted)
                throw std::runtime_error("adapter_omission_sample_save");
            const auto omission      = read(output / "omission.samples");
            const auto omissionTile  = tile(omission, x);
            const auto omissionCount = differences(omissionTile, reference);
            omissionPassed           = omissionTile == before && omissionCount != 0;
            report.insert("omission_method", "no_draw_seeded_planes");
            report.insert("omission_draw_submitted", false);
            report.insert("omission_before_equal", omissionTile == before);
            report.insert("omission_reference_changed_sample_bytes", static_cast<std::int64_t>(omissionCount));
            report.insert("omission_reference_differs", omissionCount != 0);
            report.insert("omission_control_passed", omissionPassed);
        }
        savedNative.outputPath    = output / "baseline.samples";
        const auto baselineResult = renderer.ReplayOffscreen(savedNative);
        if (!baselineResult.success)
            throw std::runtime_error(baselineResult.error);
        const auto replay = [&](const SolidDrawInputs& source, const char* name)
        {
            SolidDrawRecipe derived;
            if (!BuildSolidDrawRecipe(source, shader, derived, error))
                throw std::runtime_error(error);
            if (seeded)
                SeedSolidDrawReferenceDestination(derived.native, before, x);
            derived.native.outputPath = output / name;
            const auto result         = renderer.ReplayOffscreen(derived.native);
            if (!result.success)
                throw std::runtime_error(result.error);
            return read(derived.native.outputPath);
        };
        const auto baseline     = read(savedNative.outputPath);
        const auto baselineTile = tile(baseline, x);
        const auto count        = differences(baselineTile, reference);
        report.insert("sample_bytes_compared", static_cast<std::int64_t>(reference.size()));
        report.insert("sample_byte_differences", static_cast<std::int64_t>(count));
        report.insert("sample_buffers_equal", count == 0);
        // Owner decision 2026-10-11: fixed-function RGBA8 blending may differ
        // from the float ROV reference by 1/255, only where both draws changed
        // the byte. Coverage and every unblended byte stay exact.
        constexpr int kBlendTolerance  = 1;
        std::size_t   outsideTolerance = 0;
        int           maxDifference    = 0;
        for (std::size_t i = 0; i < reference.size(); ++i)
        {
            const int delta = std::abs(int(baselineTile[i]) - int(reference[i]));
            maxDifference   = std::max(maxDifference, delta);
            if (delta && (delta > kBlendTolerance || baselineTile[i] == before[i] || reference[i] == before[i]))
                ++outsideTolerance;
        }
        const bool parity = outsideTolerance == 0;
        report.insert("blend_tolerance", kBlendTolerance);
        report.insert("sample_byte_max_abs_difference", maxDifference);
        report.insert("sample_differences_outside_tolerance", static_cast<std::int64_t>(outsideTolerance));
        report.insert("sample_parity_within_blend_tolerance", parity);
        report.insert("live_native_replay_equal", baseline == read(capture / "native.samples"));
        report.insert("live_native_replay_equal_required", !seeded);
        bool drawEffect = true, changedPositionsEqual = true;
        if (seeded)
        {
            const auto  nativeChanged       = differences(baselineTile, before);
            const auto  referenceChanged    = differences(reference, before);
            std::size_t positionDifferences = 0;
            for (std::size_t i = 0; i < before.size(); ++i)
                positionDifferences += (baselineTile[i] != before[i]) != (reference[i] != before[i]);
            drawEffect            = nativeChanged != 0 && referenceChanged != 0;
            changedPositionsEqual = positionDifferences == 0;
            report.insert("native_draw_changed_sample_bytes", static_cast<std::int64_t>(nativeChanged));
            report.insert("reference_draw_changed_sample_bytes", static_cast<std::int64_t>(referenceChanged));
            report.insert("draw_effect_control_passed", drawEffect);
            report.insert("changed_byte_position_differences", static_cast<std::int64_t>(positionDifferences));
            report.insert("changed_byte_positions_equal", changedPositionsEqual);
            // With zero sample differences these follow from parity; the GPU
            // seed round trip is the independent no-fragment evidence.
            report.insert("controls_implied_by_parity", toml::array{ "changed_byte_positions", "omission" });
        }
        // The catalog solid VS reads c0..c3 written by 0x823038D8. Moving
        // c0.w by 0.25 clip units changes horizontal coverage, including alpha.
        constexpr std::size_t constant = 0x780 + 12;
        const auto            saved    = word(inputs.state, constant);
        const auto            changed  = std::bit_cast<std::uint32_t>(std::bit_cast<float>(saved) + 0.25F);
        for (unsigned c = 0; c < 4; ++c)
            inputs.state[constant + c] = static_cast<std::uint8_t>(changed >> (24 - c * 8));
        const auto mutation = replay(inputs, "mutation.samples");
        report.insert("mutation_shadow_offset", static_cast<std::int64_t>(constant));
        report.insert("mutation_original_word", saved);
        report.insert("mutation_changed_word", changed);
        report.insert("mutation_delta", 0.25);
        const auto mutationCount = differences(tile(mutation, x), baselineTile);
        report.insert("mutation_changed_sample_bytes", static_cast<std::int64_t>(mutationCount));
        for (unsigned c = 0; c < 4; ++c)
            inputs.state[constant + c] = static_cast<std::uint8_t>(saved >> (24 - c * 8));
        const auto restored = replay(inputs, "restored.samples");
        renderer.Shutdown();
        const bool recovered = restored == baseline;
        report.insert("restored_baseline_equal", recovered);
        report.insert("mutation_control_passed", mutationCount != 0 && recovered);
        const bool savedRecipe = report["saved_pre_original_recipe_reproduced"].value_or(false);
        const bool liveEqual   = report["live_native_replay_equal"].value_or(false);
        const bool success     = vsEqual && psEqual && geometryEqual && !mismatches && constantsEqual && originalConstantsEqual && viewportEqual &&
                                 (seeded || transparent) && parity && mutationCount && recovered && savedRecipe && (seeded ? unseededLiveEqual : liveEqual) &&
                                 destinationRoundTrip && drawEffect && changedPositionsEqual && omissionPassed;
        report.insert_or_assign("complete", success);
        error = success ? "" : "adapter_comparison_gate_failed; inspect comparison.toml";
        report.insert("error", error);
        std::ofstream file(output / "comparison.toml");
        file << report;
        file.close();
        if (!file)
            throw std::runtime_error("adapter_comparison_report_save");
        return success;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        report.insert_or_assign("error", error);
        if (reserved)
        {
            std::ofstream file(output / "comparison.toml");
            file << report;
        }
        return false;
    }
}

bool IngestSolidAdapterReference(const std::filesystem::path& capture,
                                 const std::filesystem::path& output,
                                 std::string&                 error)
{
    using namespace rerevved::gpu::guest_d3d;
    try
    {
        if (std::filesystem::exists(output))
            throw std::runtime_error("adapter_reference_output_not_fresh");
        SolidDrawInputs                                inputs;
        SolidDrawRecipe                                recipe;
        rex::graphics::diagnostic::DrawCaptureSnapshot reference;
        if (!LoadSolidDrawInputs(capture / "adapter", inputs, error) ||
            !LoadSdkSolidDrawReference(capture / "xenos", reference, error))
            return false;
        if (!inputs.inputsComplete || !inputs.solidCandidate)
            throw std::runtime_error("adapter_reference_inputs_incomplete_or_foreign");
        // Saved adapter bytes were acquired before Original. Only correlation
        // consumes Xenos bytes; no reference data is assigned to recipe.native.
        recipe.patchedVertexCode = read(capture / "adapter" / "vs-patched.be.bin");
        recipe.effectiveState    = read(capture / "adapter" / "effective-state.be.bin");
        const auto originalState = read(capture / "original-state.be.bin");
        if (originalState.size() != 0x3500)
            throw std::runtime_error("adapter_reference_original_state_extent");
        return ProcessSolidDrawReference(reference, inputs, recipe, word(originalState, 0x3488), word(originalState, 0x348C), output, true, error);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
