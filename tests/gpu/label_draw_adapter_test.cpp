#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/label_draw_adapter.h"
#include "gpu/guest_d3d/native_draw_families.h"
#include "logging_scope.hpp"
#include "stream_queue_fixture.h"

#include <rex/logging.h>

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    UNSCOPED_INFO("cannot open " + path.string());
    REQUIRE(bool(file));
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::string environment(const char* name)
{
    char       buffer[4096]{};
    const auto size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return size && size < sizeof(buffer) ? std::string(buffer) : std::string();
}

// Three stream-zero fetches whose last two records form one closing group.
SolidDrawInputs threeFetches()
{
    SolidDrawInputs in;
    in.arguments.stride = 28;
    in.state.resize(0x3500);
    in.vertexCode.resize(132, 0xA5);
    in.vertexMetadata.resize(32);
    store(in.vertexMetadata, 28, 3);
    in.fetchTemplates.resize(36);
    in.fetchRecords.resize(12);
    in.declaration.resize(36);
    in.swizzleTable.resize(16);
    in.formatTable.resize(64);
    in.formatTable[37] = 2;
    in.formatTable[6]  = 1;
    store(in.fetchRecords, 0, 0x00000003);
    store(in.fetchRecords, 4, 0x00105006);
    store(in.fetchRecords, 8, 0x0030A007);
    // Declaration elements: offset, type (format in bits 16..21 after the
    // patch's rotation), then usage and usage index at bytes 9 and 10.
    const std::array<std::array<std::uint32_t, 4>, 3> elements{ { { 0, 0x001A23A6, 0, 0 }, { 16, 0x002C23A5, 5, 0 }, { 24, 0x001A2086, 10, 0 } } };
    for (unsigned i = 0; i < 3; ++i)
    {
        store(in.declaration, i * 12, elements[i][0]);
        store(in.declaration, i * 12 + 4, elements[i][1]);
        in.declaration[i * 12 + 9]  = static_cast<std::uint8_t>(elements[i][2]);
        in.declaration[i * 12 + 10] = static_cast<std::uint8_t>(elements[i][3]);
    }
    return in;
}

// NotMine continues the chain; build/refusal ends it, even without detail.
void familyChain()
{
    std::vector<std::string_view> names;
    for (const auto& family : NativeDrawFamilyRegistry())
        names.push_back(family.option);
    UNSCOPED_INFO("the registry keeps the boot order and option names");
    REQUIRE(names == std::vector<std::string_view>{ "native_draw_panel_capture", "native_draw_label_shaders", "native_draw_gfx_shaders", "native_draw_mesh_shaders", "native_draw_menu_shaders", "native_draw_leader_shaders", "native_draw_movie_shaders", "native_draw_scene_shaders" });

    std::string tried;
    const auto  family = [&tried](char name, NativeDrawFamilyResult result, std::string reason = {})
    {
        return NativeDrawFamily([&tried, name, result, reason](const NativeDrawSubmission&, NativeDrawReplayRecipe&, std::string& detail)
                                {
                                    UNSCOPED_INFO("a family starts with an empty detail");
                                    REQUIRE(detail.empty());
                                    tried += name;
                                    detail = reason;
                                    return result;
                                });
    };
    using enum NativeDrawFamilyResult;
    NativeDrawSubmission   submission;
    NativeDrawReplayRecipe draw;
    std::string            detail = "stale";
    auto                   chain  = ComposeNativeDrawFamilies({ family('a', NotMine), family('b', NotMine), family('c', NotMine) });
    UNSCOPED_INFO("every family is tried in order when none claims the draw");
    REQUIRE((!chain(submission, draw, detail) && detail.empty() && tried == "abc"));
    tried = {};
    chain = ComposeNativeDrawFamilies({ family('a', NotMine), family('b', Built), family('c', Built) });
    UNSCOPED_INFO("the first build ends composition");
    REQUIRE((chain(submission, draw, detail) && detail.empty() && tried == "ab"));
    tried = {};
    chain = ComposeNativeDrawFamilies({ family('a', NotMine), family('b', Refused, "b_refused"), family('c', Built) });
    UNSCOPED_INFO("a refusal with a detail stops composition");
    REQUIRE((!chain(submission, draw, detail) && detail == "b_refused" && tried == "ab"));
    tried = {};
    chain = ComposeNativeDrawFamilies({ family('a', Refused), family('b', Built) });
    UNSCOPED_INFO("a claimed draw whose builder fails without a detail stops composition");
    REQUIRE((!chain(submission, draw, detail) && detail.empty() && tried == "a"));
    UNSCOPED_INFO("no families admit nothing");
    REQUIRE((!ComposeNativeDrawFamilies({})(submission, draw, detail) && detail.empty()));

    const auto reader = [](bool claimed, std::string reason, bool built)
    {
        return MakeReaderDrawFamily<int>([claimed, reason](const NativeDrawSubmission&, int& inputs, std::string& detail)
                                         {
                                             inputs = 7;
                                             detail = reason;
                                             return claimed;
                                         },
                                         [built](const int& inputs, NativeDrawReplayRecipe&, std::string& detail)
                                         {
                                             UNSCOPED_INFO("the builder receives the reader's inputs");
                                             REQUIRE(inputs == 7);
                                             if (!built)
                                                 detail = "build_refused";
                                             return built;
                                         });
    };
    const auto run = [&](const NativeDrawFamily& candidate)
    {
        detail.clear();
        return candidate(submission, draw, detail);
    };
    UNSCOPED_INFO("a reader's false without an error is not mine");
    REQUIRE((run(reader(false, {}, true)) == NotMine && detail.empty()));
    UNSCOPED_INFO("a reader's error refuses");
    REQUIRE((run(reader(false, "read_refused", true)) == Refused && detail == "read_refused"));
    UNSCOPED_INFO("a claimed draw takes the builder's refusal");
    REQUIRE((run(reader(true, {}, false)) == Refused && detail == "build_refused"));
    UNSCOPED_INFO("a claimed draw takes the builder's result");
    REQUIRE(run(reader(true, {}, true)) == Built);
    const auto silentBuilder = MakeReaderDrawFamily<int>([](const NativeDrawSubmission&, int&, std::string&)
                                                         {
                                                             return true;
                                                         },
                                                         [](const int&, NativeDrawReplayRecipe&, std::string&)
                                                         {
                                                             return false;
                                                         });
    UNSCOPED_INFO("a claimed draw whose builder fails without a detail is refused, not passed on");
    REQUIRE((run(silentBuilder) == Refused && detail.empty()));
    tried = {};
    chain = ComposeNativeDrawFamilies({ silentBuilder, family('b', Built) });
    UNSCOPED_INFO("the family after a silent builder failure is not tried");
    REQUIRE((!chain(submission, draw, detail) && tried.empty()));
    std::cout << "Draw family registry and composition order checks passed\n";
}

void unit()
{
    std::string               error;
    std::vector<std::uint8_t> patched;
    auto                      in = threeFetches();
    UNSCOPED_INFO("three fetches patch: " + error);
    REQUIRE(PatchVertexFetch(in, kLabelAttributeOffsets, patched, error));
    // Records 3, 6 and 7 name the patched instruction slots.
    UNSCOPED_INFO("declaration offsets and the stride/4 patch state reach each fetch");
    REQUIRE((word(patched, 3 * 12 + 8) == 7 && word(patched, 6 * 12 + 8) == 0x407 && word(patched, 7 * 12 + 8) == 0x607));
    UNSCOPED_INFO("the closing group coalesces UV and color into one three-dword fetch; position stays alone");
    REQUIRE(((word(patched, 6 * 12) >> 27 & 7) == 2 && (word(patched, 7 * 12 + 4) & 0x40000000) &&
             !(word(patched, 3 * 12) >> 27 & 7) && !(word(patched, 3 * 12 + 4) & 0x40000000)));
    UNSCOPED_INFO("only metadata-designated fetch instructions change");
    REQUIRE((std::equal(patched.begin(), patched.begin() + 36, in.vertexCode.begin()) &&
             std::equal(patched.begin() + 48, patched.begin() + 72, in.vertexCode.begin() + 48) &&
             std::equal(patched.begin() + 96, patched.end(), in.vertexCode.begin() + 96)));
    store(in.declaration, 12, 20);
    UNSCOPED_INFO("an unadmitted UV offset has a named rejection");
    REQUIRE((!PatchVertexFetch(in, kLabelAttributeOffsets, patched, error) && error == "adapter_attribute_offset"));
    store(in.declaration, 12, 16);
    UNSCOPED_INFO("the solid offsets do not admit the label layout");
    REQUIRE((!PatchVertexFetch(in, kSolidAttributeOffsets, patched, error) && error == "adapter_attribute_offset"));
    store(in.vertexMetadata, 28, 33);
    UNSCOPED_INFO("the fetch count comes from vertexMetadata+28 and is bounded");
    REQUIRE((!PatchVertexFetch(in, kLabelAttributeOffsets, patched, error) && error == "adapter_fetch_count"));

    // The captured glyph-cache fetch: linear k_8 1024x1024, one level,
    // linear filters, repeat addressing and a -0.5 LOD bias.
    const std::array<std::uint32_t, 6> fetch{ 0x08000002, 0x1FB17002, 0x007FE3FF, 0x00A80248, 0x003F0003, 0x00000218 };
    NativeDrawReplaySampler            sampler;
    UNSCOPED_INFO("glyph sampler: " + error);
    REQUIRE(DecodeLabelSampler(fetch, sampler, error));
    UNSCOPED_INFO("repeat axes, linear filters and an unclamped LOD without bias");
    REQUIRE((sampler.address == std::array<std::uint32_t, 3>{ 1, 1, 3 } && sampler.magLinear && sampler.minLinear &&
             sampler.mipLinear && !sampler.anisotropy && sampler.minLod == 0 &&
             sampler.maxLod == std::numeric_limits<float>::max() && sampler.mipBias == 0));
    auto mipped = fetch;
    mipped[4] |= 1U << 6;
    UNSCOPED_INFO("a second level is rejected by name");
    REQUIRE((!DecodeLabelSampler(mipped, sampler, error) && error == "label_texture_mip_levels"));
    auto aniso = fetch;
    aniso[3] |= 1U << 25;
    UNSCOPED_INFO("anisotropy is rejected");
    REQUIRE((!DecodeLabelSampler(aniso, sampler, error) && error == "label_sampler_unsupported"));
    // Mip filter base map (2), the movie planes' form, samples the one level;
    // mip filter 3 (use the fetch constant) stays refused.
    auto baseMap = fetch;
    baseMap[3]   = (baseMap[3] & ~(3U << 23)) | (2U << 23);
    UNSCOPED_INFO("a base-map mip filter on one level is admitted without a mip filter: " + error);
    REQUIRE((DecodeLabelSampler(baseMap, sampler, error) && sampler.magLinear && sampler.minLinear && !sampler.mipLinear &&
             sampler.address == std::array<std::uint32_t, 3>{ 1, 1, 3 }));
    auto fetchMip = fetch;
    fetchMip[3]   = fetchMip[3] | (3U << 23);
    UNSCOPED_INFO("mip filter 3 is rejected");
    REQUIRE((!DecodeLabelSampler(fetchMip, sampler, error) && error == "label_sampler_unsupported"));
    auto mippedBaseMap = baseMap;
    mippedBaseMap[4] |= 1U << 6;
    UNSCOPED_INFO("a base-map mip filter still needs one level");
    REQUIRE((!DecodeLabelSampler(mippedBaseMap, sampler, error) && error == "label_texture_mip_levels"));

    // Composition: a family without a shader has no native route.
    unsigned   reads   = 0;
    ReadMemory failing = [&](std::uint64_t, std::uint64_t, bool)
    {
        ++reads;
        return std::vector<std::uint8_t>{};
    };
    NativeDrawSubmission label;
    label.hook      = NativeDrawHook::NonindexedUp;
    label.caller    = kLabelDrawCaller;
    label.arguments = { 0x40000000, 4, 288, 0x50000000, 28, 0, 0, 0 };
    NativeDrawSubmission panel;
    panel.hook   = NativeDrawHook::IndexedUp;
    panel.caller = 0x82303E90;
    NativeDrawReplayRecipe draw;
    std::string            detail;
    auto                   none = MakeBootDrawBuilder(failing, nullptr, nullptr);
    UNSCOPED_INFO("no shaders: nothing is read and nothing is admitted");
    REQUIRE((!none(label, draw, detail) && detail.empty() && !none(panel, draw, detail) && detail.empty() && reads == 0));
    const auto shader = std::make_shared<NativeGameplayShaderInterface>();
    auto       labels = MakeBootDrawBuilder(failing, nullptr, shader);
    UNSCOPED_INFO("a label-only builder leaves the panel without a route");
    REQUIRE((!labels(panel, draw, detail) && detail.empty() && reads == 0));
    UNSCOPED_INFO("an unreadable label candidate is rejected with a reason");
    REQUIRE((!labels(label, draw, detail) && !detail.empty()));
    reads     = 0;
    auto both = MakeBootDrawBuilder(failing, shader, shader);
    UNSCOPED_INFO("the panel reader's rejection stops composition");
    REQUIRE((!both(panel, draw, detail) && !detail.empty() && reads == 1));
    NativeDrawSubmission foreign = label;
    foreign.caller               = 0x82304000;
    reads                        = 0;
    UNSCOPED_INFO("a foreign nonindexed-UP call is outside both families");
    REQUIRE((!both(foreign, draw, detail) && detail.empty() && reads == 0));
    std::cout << "Label patch, sampler and builder composition checks passed\n";
}

NativeFrameSwap swapRecord()
{
    NativeFrameSwap swap;
    swap.sequence            = 1;
    swap.numberedThrough     = 0;
    swap.frontbufferPhysical = 0x1F000000;
    // Identity output swizzle X,Y,Z,W.
    swap.fetch         = { 2, 0x1F000000 | 6, 1279 | (719U << 13), 0xD10, 0, 0 };
    swap.textureFormat = 6;
    swap.width         = 1280;
    swap.height        = 720;
    for (std::uint32_t i = 0; i < swap.gammaTable.size(); ++i)
    {
        const auto value   = (i * 1023U + 127U) / 255U;
        swap.gammaTable[i] = (value << 20) | (value << 10) | value;
    }
    return swap;
}

constexpr std::array<float, 4> kClearColor{ 0, 0, 0, 1 };

// The control replays the capture's Original outputs directly: the guest's
// own patched VS and effective state, its texture fetch and bytes. It shares
// no code with the reader, fold, fetch patch, fixed-state check or assembler.
NativeDrawReplayRecipe controlLabel(const std::filesystem::path& capture, const NativeGameplayShaderInterface& shader, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler)
{
    const auto                   state    = read(capture / "original-state.be.bin");
    const auto                   vertices = read(capture / "original-vertices.be.bin");
    const auto                   fetchRaw = read(capture / "adapter" / "texture-fetch.be.bin");
    const auto                   table    = toml::parse_file((capture / "adapter" / "recipe.toml").string());
    const auto                   source   = table["source_vertices"].value<std::uint32_t>().value();
    std::array<std::uint32_t, 6> fetch{};
    for (std::size_t i = 0; i < fetch.size(); ++i)
        fetch[i] = word(fetchRaw, i * 4);
    std::string error;
    UNSCOPED_INFO("control texture: " + error);
    REQUIRE(DecodeNativeTexture(fetch, read(capture / "adapter" / "texture.bin"), texture, error));
    sampler           = {};
    sampler.address   = { 1, 1, 3 };
    sampler.magLinear = sampler.minLinear = sampler.mipLinear = true;
    sampler.maxLod                                            = std::numeric_limits<float>::max();
    std::vector<std::uint32_t> registers(0x5003);
    const auto                 copy = [&](unsigned from, unsigned to, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
            registers[to + i] = word(state, from + i * 4);
    };
    for (const auto [from, to, count] : { std::array<unsigned, 3>{ 0x2880, 0x2000, 16 }, { 0x28C0, 0x2080, 3 }, { 0x28CC, 0x2100, 21 }, { 0x2920, 0x2180, 5 }, { 0x2934, 0x2200, 12 }, { 0x29B8, 0x2300, 38 }, { 0x780, 0x4000, 1024 }, { 0x1780, 0x4400, 896 }, { 0x2780, 0x4900, 40 } })
        copy(from, to, count);
    const auto base   = (source & 0x1FFFFFFFU) + (((source >> 20) + 512) & 0x1000);
    registers[0x48BE] = base | 3;
    registers[0x48BF] = static_cast<std::uint32_t>(vertices.size()) | 0x10000002U;
    const auto words  = [&](const std::vector<std::uint8_t>& bytes)
    {
        std::vector<std::uint32_t> result(bytes.size() / 4);
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = word(bytes, i * 4);
        return result;
    };
    const auto                                         vs = words(read(capture / "original-vs.be.bin")), ps = words(read(capture / "adapter" / "ps.be.bin"));
    const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, vertices } } };
    const std::array<NativeGameplayTextureBinding, 1>  bindings{ { { 0, &texture, &sampler } } };
    NativeGameplayDrawView                             view;
    view.registers       = registers;
    view.vertexMicrocode = vs;
    view.pixelMicrocode  = ps;
    view.vertexFetches   = fetches;
    view.textureBindings = bindings;
    view.primitive       = 4;
    view.indexCount      = static_cast<std::uint32_t>(vertices.size() / 28);
    view.indexed         = false;
    NativeGameplayDrawTarget target;
    target.fullViewportTarget = true;
    target.width              = 1280;
    target.height             = 720;
    target.viewport           = { 0, 0, 1280, 720, 0, 0 };
    target.scissor            = { 0, 0, 1280, 720 };
    NativeDrawReplayRecipe recipe;
    UNSCOPED_INFO("control label recipe: " + error);
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error));
    return recipe;
}

struct Mutation
{
    unsigned labels      = 12;
    bool     uvOffset20  = false;
    bool     swizzleWToX = false;
};

int c1(const std::filesystem::path& capture)
{
    const auto solidCapture = environment("REREVVED_SOLID_ADAPTER_CAPTURE");
    const auto catalog      = environment("REREVVED_LABEL_SHADERS");
    UNSCOPED_INFO("the label checks also need REREVVED_SOLID_ADAPTER_CAPTURE and REREVVED_LABEL_SHADERS");
    REQUIRE((!solidCapture.empty() && !catalog.empty()));
    std::string     error;
    LabelDrawInputs inputs;
    UNSCOPED_INFO("C1 inputs: " + error);
    REQUIRE((LoadLabelDrawInputs(capture / "adapter", inputs, error) && inputs.labelCandidate && inputs.inputsComplete));
    const auto originalVs    = read(capture / "original-vs.be.bin");
    const auto originalState = read(capture / "original-state.be.bin");

    // Patch: the native label fetch patch equals the guest's patched VS, and
    // the solid patch still equals its saved M1a words.
    std::vector<std::uint8_t> code = inputs.vertexCode, folded;
    UNSCOPED_INFO("label fold: " + error);
    REQUIRE(FoldSolidShaderMetadata(inputs, code, folded, error));
    SolidDrawInputs linked = inputs;
    linked.vertexCode      = code;
    std::vector<std::uint8_t> patched;
    UNSCOPED_INFO("label patch equals C1 original-vs: " + error);
    REQUIRE((PatchVertexFetch(linked, kLabelAttributeOffsets, patched, error) && patched == originalVs));
    SolidDrawInputs solid;
    UNSCOPED_INFO("M1a inputs: " + error);
    REQUIRE((LoadSolidDrawInputs(std::filesystem::path(solidCapture) / "adapter", solid, error) && solid.solidCandidate));
    NativeGameplayShaderInterface panelShader;
    UNSCOPED_INFO("panel shader: " + error);
    REQUIRE(LoadSolidPanelShader(solidCapture, panelShader, error));
    SolidDrawRecipe panel;
    UNSCOPED_INFO("solid patched words unchanged: " + error);
    REQUIRE((BuildSolidDrawRecipe(solid, panelShader, panel, error) && panel.patchedVertexCode == read(std::filesystem::path(solidCapture) / "adapter" / "vs-patched.be.bin")));
    std::cout << "Label fetch patch equals the guest's patched VS; solid patch unchanged\n";

    // Inputs: the recipe consumes the guest's own Original outputs.
    LabelDrawShaders labelShaders;
    UNSCOPED_INFO("label catalog: " + error);
    REQUIRE(LoadLabelDrawShaders(catalog, labelShaders, error));
    const auto& labelShader = labelShaders.presented;
    // Signedness is part of the pinned vertex layout: a flipped flag changes how the color bytes decode.
    for (const std::size_t flipped : { std::size_t(0), std::size_t(2) })
    {
        const auto copy = std::filesystem::temp_directory_path() / ("label-catalog-signed-" + std::to_string(flipped));
        std::filesystem::remove_all(copy);
        std::filesystem::copy(catalog, copy, std::filesystem::copy_options::recursive);
        const auto  tomlPath = copy / "shaders.toml";
        std::string text;
        {
            std::ifstream in(tomlPath, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::size_t at = 0;
        for (std::size_t i = 0; i <= flipped; ++i)
            at = text.find("signed = ", at + 1);
        UNSCOPED_INFO("signed flag found in the catalog copy");
        REQUIRE(at != std::string::npos);
        text.replace(at + 9, text[at + 9] == 't' ? 4 : 5, text[at + 9] == 't' ? "false" : "true");
        {
            std::ofstream out(tomlPath, std::ios::binary | std::ios::trunc);
            out << text;
        }
        NativeGameplayShaderInterface rejected;
        std::string                   rejectedError;
        UNSCOPED_INFO("a flipped signed flag on attribute " + std::to_string(flipped) + " is rejected by name: " + rejectedError);
        REQUIRE((!LoadLabelDrawShader(copy, rejected, rejectedError) && rejectedError == "label_catalog_layout"));
        std::filesystem::remove_all(copy);
    }
    LabelDrawRecipe recipe;
    UNSCOPED_INFO("label recipe: " + error);
    REQUIRE(BuildLabelDrawRecipe(inputs, labelShaders, recipe, error));
    UNSCOPED_INFO("shader words equal C1");
    REQUIRE(recipe.patchedVertexCode == originalVs);
    const auto originalRegisters = GuestDrawRegisters(originalState);
    UNSCOPED_INFO("effective registers and constants equal C1's Original state");
    REQUIRE(GuestDrawRegisters(recipe.effectiveState) == originalRegisters);
    const auto& native = recipe.native;
    UNSCOPED_INFO("catalog DXIL bound");
    REQUIRE((std::ranges::equal(native.vertexShaderBytes(), labelShader.vertexShaderDxil) && std::ranges::equal(native.pixelShaderBytes(), labelShader.pixelShaderDxil)));
    // Recipe constants keep the little-endian register bits.
    std::vector<std::uint8_t> constants(256 * 16);
    std::memcpy(constants.data(), originalRegisters.data() + 0x4000, constants.size());
    UNSCOPED_INFO("vertex constants equal C1");
    REQUIRE(native.vertexConstants == constants);
    const auto vertices = read(capture / "original-vertices.be.bin");
    UNSCOPED_INFO("geometry equals C1's 288 vertices");
    REQUIRE((inputs.vertices == vertices && native.indexCount == 288 && native.vertexStrideBytes == 48));
    for (std::size_t v = 0; v < 288; ++v)
        for (unsigned c = 0; c < 6; ++c)
        {
            // Position xyzw then UV xy, big-endian floats in the guest vertex.
            const auto    source = word(vertices, v * 28 + c * 4);
            std::uint32_t decoded;
            std::memcpy(&decoded, native.vertexBytes().data() + v * 48 + (c < 4 ? c * 4 : 16 + (c - 4) * 4), 4);
            UNSCOPED_INFO("decoded position and UV equal the guest floats");
            REQUIRE(decoded == source);
        }
    UNSCOPED_INFO("the linear k_8 glyph cache decodes to its own bytes");
    REQUIRE((std::ranges::equal(recipe.texture.payloadBytes(), read(capture / "adapter" / "texture.bin")) && recipe.texture.width == 1024 &&
             recipe.texture.height == 1024));
    UNSCOPED_INFO("sampler bound at slot 0");
    REQUIRE((native.samplers.size() == 1 && native.samplers[0].address == std::array<std::uint32_t, 3>{ 1, 1, 3 } && native.samplers[0].minLinear));
    auto broken = inputs;
    store(broken.state, 0x2880, 0x0A010500);
    UNSCOPED_INFO("an unpinned surface is rejected: " + error);
    REQUIRE((!BuildLabelDrawRecipe(broken, labelShaders, recipe, error) && error.starts_with("label_fixed_state 0x2000")));
    broken = inputs;
    broken.textureFetch[4] |= 1U << 6;
    store(broken.state, 0x480 + 16, broken.textureFetch[4]);
    UNSCOPED_INFO("a mip chain is rejected: " + error);
    REQUIRE((!BuildLabelDrawRecipe(broken, labelShaders, recipe, error) && error == "label_texture_mip_levels"));
    // Display rectangles, as for GFx: the game-start label's 640x720 window
    // scissor inside the full viewport builds, and other forms fail by name.
    {
        const auto rectangle = [&](std::uint32_t topLeft, std::uint32_t bottomRight, std::array<float, 4> viewport)
        {
            auto in = inputs;
            store(in.state, 0x28C4, topLeft);
            store(in.state, 0x28C8, bottomRight);
            store(in.state, 0x2908, std::bit_cast<std::uint32_t>(viewport[2] / 2));
            store(in.state, 0x290C, std::bit_cast<std::uint32_t>(viewport[0] + viewport[2] / 2));
            store(in.state, 0x2910, std::bit_cast<std::uint32_t>(-viewport[3] / 2));
            store(in.state, 0x2914, std::bit_cast<std::uint32_t>(viewport[1] + viewport[3] / 2));
            for (unsigned i = 0; i < viewport.size(); ++i)
                store(in.state, 0x3168 + i * 4, std::bit_cast<std::uint32_t>(viewport[i]));
            return in;
        };
        LabelDrawRecipe inset;
        UNSCOPED_INFO("an inset window scissor builds: " + error);
        REQUIRE((BuildLabelDrawRecipe(rectangle(0, 0x02D00280, { 0, 0, 1280, 720 }), labelShaders, inset, error) && inset.native.viewport.width == 1280 &&
                 inset.native.scissor.left == 0 && inset.native.scissor.top == 0 && inset.native.scissor.right == 640 && inset.native.scissor.bottom == 720));
        const std::array<std::pair<LabelDrawInputs, std::string>, 4> refusals{ {
            { rectangle(0x80000000, 0x02D00500, { 0, 0, 1280, 720 }), "label_scissor_form 0x2081: 0x80000000" },
            { rectangle(0x00000280, 0x02D00000, { 0, 0, 1280, 720 }), "label_scissor_form 0x2082: 0x2d00000" },
            { rectangle(0, 0x02D00500, { 0, 0, 0, 720 }), "label_viewport_form: unsupported gameplay viewport scale or depth range" },
            { rectangle(0, 0x02D00500, { 0, 0, 640, 720 }), "label_scissor_beyond_viewport: scissor 0,0,1280,720 viewport 0,0 640x720" },
        } };
        for (const auto& [in, expected] : refusals)
        {
            UNSCOPED_INFO("display rectangle refused as " + expected + ": " + error);
            REQUIRE((!BuildLabelDrawRecipe(in, labelShaders, inset, error) && error == expected));
        }

        // Census B8: the 256x32 single-sample off-screen form, with the whole
        // target as viewport and window scissor, builds with a 1X entry.
        const auto offscreen = [&](std::uint32_t depth, std::uint32_t bottomRight, float viewportWidth = 256)
        {
            auto in = rectangle(0, bottomRight, { 0, 0, viewportWidth, 32 });
            store(in.state, 0x2880, kLabelOffscreenForm.surfaceInfo);
            store(in.state, 0x2888, depth);
            return in;
        };
        const auto form = offscreen(kLabelOffscreenForm.depthInfo, kLabelOffscreenForm.scissor);
        UNSCOPED_INFO("the off-screen form without a 1X entry is refused by name: " + error);
        REQUIRE((!BuildLabelDrawRecipe(form, LabelDrawShaders{ labelShader }, inset, error) && error == "label_offscreen_unpinned"));
        auto single        = labelShader;
        single.sampleCount = single.sampleMask = 1;
        const LabelDrawShaders both{ labelShader, single };
        UNSCOPED_INFO("the off-screen form builds at one sample: " + error);
        REQUIRE((BuildLabelDrawRecipe(form, both, inset, error) && inset.native.width == 256 && inset.native.height == 32 && inset.native.sampleCount == 1 &&
                 inset.native.scissor.right == 256 && inset.native.scissor.bottom == 32 && inset.native.viewport.width == 256));
        UNSCOPED_INFO("the off-screen surface with the presented depth is refused: " + error);
        REQUIRE((!BuildLabelDrawRecipe(offscreen(0x530, kLabelOffscreenForm.scissor), both, inset, error) && error == "label_fixed_state 0x2002: 0x530"));
        UNSCOPED_INFO("a partial off-screen scissor is refused: " + error);
        REQUIRE((!BuildLabelDrawRecipe(offscreen(kLabelOffscreenForm.depthInfo, 0x00200080), both, inset, error) &&
                 error == "label_offscreen_viewport: scissor 0x0 0x200080 viewport 0,0 256x32"));
        UNSCOPED_INFO("a viewport wider than the off-screen target is refused: " + error);
        REQUIRE((!BuildLabelDrawRecipe(offscreen(kLabelOffscreenForm.depthInfo, kLabelOffscreenForm.scissor, 512), both, inset, error) &&
                 error == "label_offscreen_viewport: scissor 0x0 0x200100 viewport 0,0 512x32"));
        if (const auto offscreenCatalog = environment("REREVVED_LABEL_OFFSCREEN_SHADERS"); !offscreenCatalog.empty())
        {
            LabelDrawShaders pinned;
            UNSCOPED_INFO("the off-screen label catalog has both entries: " + error);
            REQUIRE((LoadLabelDrawShaders(offscreenCatalog, pinned, error) && pinned.offscreen && pinned.offscreen->sampleCount == 1 &&
                     pinned.presented.pixelGuestWordDigest == labelShader.pixelGuestWordDigest));
            UNSCOPED_INFO("the off-screen form binds the catalog's 1X pixel shader: " + error);
            REQUIRE((BuildLabelDrawRecipe(form, pinned, inset, error) && std::ranges::equal(inset.native.pixelShaderBytes(), pinned.offscreen->pixelShaderDxil)));
            std::cout << "Label off-screen form builds with " << offscreenCatalog << '\n';
        }

        // A label inside a Scaleform mask (world harvest frame
        // 007561C9EFB23973-0, key 9E3FD59C) tests stencil equal to the mask
        // depth with full masks and keeps it, on the presented form only.
        const auto masked = [&](LabelDrawInputs in, std::uint32_t refMask)
        {
            store(in.state, 0x2934, kLabelMaskedDepthControl);
            store(in.state, 0x2900, refMask);
            return in;
        };
        for (const std::uint32_t reference : { 1U, 2U })
        {
            UNSCOPED_INFO("a masked label builds: " + error);
            REQUIRE(BuildLabelDrawRecipe(masked(inputs, kLabelStencilMasks | reference), labelShaders, inset, error));
            const auto& stencil = inset.native.stencil;
            const auto  keeps   = [](const NativeDrawReplayStencilState::Face& face)
            {
                return face.compare == 3 && face.fail == 1 && face.depthFail == 1 && face.pass == 1;
            };
            UNSCOPED_INFO(std::format("a masked label tests stencil equal to {} and keeps it", reference));
            REQUIRE((stencil.enabled && stencil.reference == reference && stencil.readMask == 0xFF && stencil.writeMask == 0xFF && keeps(stencil.front) &&
                     keeps(stencil.back) && !inset.native.depth.enabled && !inset.native.depth.writeEnabled));
        }
        UNSCOPED_INFO("a masked label without full stencil masks is refused: " + error);
        REQUIRE((!BuildLabelDrawRecipe(masked(inputs, 0x00FF0F01), labelShaders, inset, error) && error == "label_stencil_mask 0x210d: 0xff0f01"));
        UNSCOPED_INFO("a masked label off the presented form is refused: " + error);
        REQUIRE((!BuildLabelDrawRecipe(masked(form, kLabelStencilMasks | 1), both, inset, error) &&
                 error == std::format("label_fixed_state depth 0x24f00271 color {:#x}", word(form.state, 0x293C))));
    }
    std::cout << "Label recipe inputs equal C1's Original outputs\n";

    const auto result = toml::parse_file((capture / "result.toml").string());
    std::cout << "Xenos reference (informational): " << result["xenos_status"].value_or(std::string("missing")) << " "
              << result["xenos_error"].value_or(std::string()) << "\n";

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the label frame was not rendered\n";
        return 77;
    }

    // Acceptance: clear, panel and twelve label batches through the assembler
    // equal a full-width control frame built directly from the saved inputs.
    GuestClear clear;
    clear.target.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    clear.flags           = 0x3F;
    clear.clear.rectangle = { 0, 0, 1280, 720 };
    clear.clear.color = clear.clear.depth = clear.clear.stencil = true;
    clear.clear.colorValue                                      = kClearColor;
    // The boot's presented clear names the bound depth surface (B+0x1C base
    // 0x530, as the 8b fixtures use); both captured draws leave depth and
    // stencil disabled, so no draw needs the identity check.
    clear.depth          = GuestDepthSurface{ 0x00700000, 0x530, 0x530, 1280, 720, 4, false };
    const auto assembled = [&](const Mutation& mutation, std::uint64_t& textureVersions)
    {
        LabelDrawShaders shader{ labelShader };
        if (mutation.uvOffset20)
            shader.presented.vertexAttributes[1].offsetBytes = 20;
        LabelTextureCache          cache;
        NativeFrameAssemblerConfig config;
        config.readback = true;
        config.build    = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            // The captures stand in for the guest reads made before Original.
            if (submission.hook == NativeDrawHook::IndexedUp)
                return BuildSolidPanelDraw(solid, panelShader, draw, detail);
            if (!BuildLabelDraw(inputs, shader, draw, detail, &cache))
                return false;
            if (mutation.swizzleWToX)
                draw.textures[0].swizzle = { 0, 4, 4, 4 };
            return true;
        };
        NativeFrameAssembler assembler(config);
        std::string          detail;
        UNSCOPED_INFO("clear admitted: " + detail);
        REQUIRE(assembler.Clear(clear, detail) != 0);
        const auto submit = [&](NativeDrawSubmission submission, std::uint64_t sequence)
        {
            submission.stateRead = true;
            submission.sequence  = sequence;
            assembler.Began(submission);
            const auto verdict = assembler.Admit(submission);
            UNSCOPED_INFO("draw admitted");
            REQUIRE(!verdict);
            assembler.Recorded(submission, verdict);
        };
        NativeDrawSubmission panelDraw;
        const auto&          p = solid.arguments;
        panelDraw.hook         = NativeDrawHook::IndexedUp;
        panelDraw.caller       = static_cast<std::uint32_t>(p.caller);
        panelDraw.stack        = 0x70000000;
        panelDraw.arguments    = { p.graphics, p.primitive, p.minimumVertex, p.vertexCount, p.indexCount, p.indices, p.indexFormat, p.vertices };
        submit(panelDraw, 1);
        const auto&          l = inputs.arguments;
        NativeDrawSubmission labelDraw;
        labelDraw.hook      = NativeDrawHook::NonindexedUp;
        labelDraw.caller    = kLabelDrawCaller;
        labelDraw.arguments = { l.graphics, l.primitive, l.vertexCount, l.vertices, l.stride, 0, 0, 0 };
        for (unsigned i = 0; i < mutation.labels; ++i)
            submit(labelDraw, 2 + i);
        // A modeled scanout of the cleared target in the title's 1280x720
        // view, unswapped so the identity fetch swizzle stands.
        GuestTarget scanned;
        scanned.native = clear.target.native;
        GuestResolveCopy scan;
        scan.copyControl = 0x00100040;
        scan.destBase    = swapRecord().frontbufferPhysical;
        scan.destPitch   = 0x02D00500;
        scan.destInfo    = 0x300;
        scan.rectWidth   = 1280;
        scan.rectHeight  = 720;
        bool partial     = false;
        UNSCOPED_INFO("the frame is scanned out: " + detail);
        REQUIRE(assembler.Resolve(scanned, scan, true, partial, detail) != 0);
        auto swap            = swapRecord();
        swap.numberedThrough = 1 + mutation.labels;
        auto frame           = assembler.Swap(swap);
        UNSCOPED_INFO("one presented pass with the panel and labels: " + frame.detail);
        REQUIRE((frame.status == NativeFrameStatus::Provisional && frame.frame && frame.frame->passes.size() == 1 &&
                 frame.frame->passes[0].DrawCount() == 1 + mutation.labels && frame.frame->outputSwizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));
        std::vector<const void*> versions;
        for (std::size_t i = 1; i < frame.frame->passes[0].DrawCount(); ++i)
            versions.push_back(frame.frame->passes[0].Draw(i).textures[0].textureVersion.get());
        std::ranges::sort(versions);
        textureVersions = static_cast<std::uint64_t>(std::ranges::unique(versions).begin() - versions.begin());
        return std::move(*frame.frame);
    };

    NativeDrawReplayTexture controlTexture;
    NativeDrawReplaySampler controlSampler;
    const auto              control = controlLabel(capture, labelShader, controlTexture, controlSampler);
    NativeRenderFrame       controlFrame;
    controlFrame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    NativeRenderFramePass pass;
    pass.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    NativeRenderFramePass::Clear full;
    full.rectangle = { 0, 0, 1280, 720 };
    full.color = full.depth = full.stencil = true;
    full.colorValue                        = kClearColor;
    pass.clears.push_back(full);
    pass.draws.push_back(panel.native);
    for (unsigned i = 0; i < 12; ++i)
        pass.draws.push_back(control);
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = 1280;
    resolve.height    = 720;
    resolve.samples   = { 0, 3 };
    pass.resolves.push_back(resolve);
    controlFrame.passes.push_back(std::move(pass));
    controlFrame.outputTextureId      = 1;
    controlFrame.gammaTable           = swapRecord().gammaTable;
    controlFrame.returnResolvedOutput = true;
    auto panelOnly                    = controlFrame;
    panelOnly.passes[0].draws.resize(1);

    std::uint64_t versions = 0;
    auto          accepted = assembled({}, versions);
    UNSCOPED_INFO("twelve batches with equal glyph bytes share one decoded texture version");
    REQUIRE(versions == 1);
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto                            expected = renderer.SubmitRenderFrame(std::move(controlFrame));
    const auto                            base     = renderer.SubmitRenderFrame(std::move(panelOnly));
    const auto                            actual   = renderer.SubmitRenderFrame(std::move(accepted));
    std::array<NativeDrawReplayResult, 3> mutated;
    const std::array<Mutation, 3>         mutations{ { { 12, true, false }, { 11, false, false }, { 12, false, true } } };
    for (std::size_t i = 0; i < mutations.size(); ++i)
        mutated[i] = renderer.SubmitRenderFrame(assembled(mutations[i], versions));
    renderer.Shutdown();
    UNSCOPED_INFO("renders: " + expected.error + base.error + actual.error);
    REQUIRE((expected.success && base.success && actual.success));
    std::size_t lit = 0;
    for (std::size_t i = 0; i + 3 < expected.resolvedOutput.size(); i += 4)
        lit += !std::equal(expected.resolvedOutput.begin() + i, expected.resolvedOutput.begin() + i + 4, base.resolvedOutput.begin() + i);
    std::cout << "Label pixels over the panel: " << lit << "\n";
    UNSCOPED_INFO("the control labels change the panel frame");
    REQUIRE(lit > 1000);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i + 3 < expected.resolvedOutput.size(); i += 4)
        mismatches += !std::equal(expected.resolvedOutput.begin() + i, expected.resolvedOutput.begin() + i + 4, actual.resolvedOutput.begin() + i);
    UNSCOPED_INFO("assembled label frame differs from the full-width control at " + std::to_string(mismatches) + " pixels");
    REQUIRE((actual.resolvedOutput.size() == expected.resolvedOutput.size() && mismatches == 0));
    constexpr std::array<const char*, 3> names{ "UV offset 16 to 20", "an omitted label", "texture swizzle W to X" };
    for (std::size_t i = 0; i < mutated.size(); ++i)
    {
        UNSCOPED_INFO(std::string(names[i]) + " render: " + mutated[i].error);
        REQUIRE(mutated[i].success);
        UNSCOPED_INFO(std::string(names[i]) + " leaves the control comparison passing");
        REQUIRE(mutated[i].resolvedOutput != expected.resolvedOutput);
    }
    std::cout << "Assembled clear, panel and twelve labels equal the full-width control bit-exactly; mutations fail it\n";
    return 0;
}

} // namespace

// A check returns 77 when its D3D12 part could not run.
static void skipIfUnavailable(int status, const char* message)
{
    if (status == 77)
    {
        SKIP(message);
    }
}

TEST_CASE("label_draw_adapter", "[ctest][label-draw-adapter][timeout-180]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
    familyChain();
}

TEST_CASE("label_draw_adapter_capture", "[.][renderer-research][label-draw-adapter][native_gpu][timeout-180]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset capture skips; a set but unusable path fails.
    const auto capture = environment("REREVVED_LABEL_ADAPTER_CAPTURE");
    if (capture.empty())
    {
        SKIP("Set REREVVED_LABEL_ADAPTER_CAPTURE to the label capture root to run this check");
    }
    skipIfUnavailable(c1(capture), "no D3D12 device for the label capture");
}
