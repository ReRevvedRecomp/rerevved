#include "gpu/d3d12/native_gameplay_clear.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/gfx_draw_inputs.h"
#include "gpu/guest_d3d/movie_draw_adapter.h"
#include "logging_scope.hpp"
#include "resource_mirror_fixture.h"
#include "stream_queue_fixture.h"

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/graphics_util.h>

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
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

std::uint32_t little(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

std::string environment(const char* name)
{
    char       buffer[4096]{};
    const auto size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return size && size < sizeof(buffer) ? std::string(buffer) : std::string();
}

// Xenos register indices read when attributing a frame's events.
constexpr std::uint32_t kRingWritePointer = 0x01C5, kSurfaceInfo = 0x2000, kColorInfo = 0x2001, kWindowOffset = 0x2080, kWindowScissorTl = 0x2081,
                        kWindowScissorBr = 0x2082, kColorMask = 0x2104, kDepthControl = 0x2200, kBlendControl0 = 0x2201, kColorControl = 0x2202,
                        kSuModeControl = 0x2205, kModeControl = 0x2208, kCopyControl = 0x2318, kCopyDestBase = 0x2319, kCopyDestPitch = 0x231A,
                        kCopyDestInfo = 0x231B, kDepthClear = 0x231D, kColorClear = 0x231E, kColorClearLow = 0x231F, kProgramControl = 0x2180;

// Memory exports include allocs, exec-clause ALU exports and out-of-code
// clauses.
bool exportsMemory(std::span<const std::uint32_t> ucode)
{
    using namespace rex::graphics::ucode;
    std::size_t                             end = ucode.size() / 3;
    std::vector<ControlFlowExecInstruction> clauses;
    for (std::size_t slot = 0; slot < end; ++slot)
    {
        ControlFlowInstruction pair[2];
        UnpackControlFlowInstructions(ucode.data() + slot * 3, pair);
        for (const auto& instruction : pair)
            if (IsControlFlowOpcodeExec(instruction.opcode()))
            {
                end = std::min<std::size_t>(end, instruction.exec.address());
                clauses.push_back(instruction.exec);
            }
            else if (instruction.opcode() == ControlFlowOpcode::kAlloc && instruction.alloc.alloc_type() == AllocType::kMemory)
                return true;
    }
    for (const auto& clause : clauses)
        for (std::uint32_t i = 0; i < clause.count(); ++i)
        {
            const std::size_t slot = clause.address() + i;
            if ((slot + 1) * 3 > ucode.size())
                return true;
            // Sequence bit 0 of each pair marks a fetch.
            if ((clause.sequence() >> (i * 2)) & 1)
                continue;
            AluInstruction alu;
            std::memcpy(&alu, ucode.data() + slot * 3, sizeof(alu));
            if (alu.is_export() && alu.vector_dest() >= std::uint32_t(ExportRegister::kExportAddress) &&
                alu.vector_dest() <= std::uint32_t(ExportRegister::kExportData4))
                return true;
        }
    return false;
}

// replacesColor requires full RT0 mask, non-killing color/depth paths, and
// always-pass depth/stencil.
bool replacesColor(std::span<const std::uint32_t> r)
{
    const auto always = [](std::uint32_t function)
    {
        return (function & 7) == 7;
    };
    const auto depth = r[kDepthControl], color = r[kColorControl];
    return (r[kModeControl] & 7) == 4 && ((r[kProgramControl] >> 24) & 7) == 0 && (r[kSuModeControl] & 3) == 0 && (r[kColorMask] & 0xF) == 0xF &&
           (!(color & 8) || always(color)) && !(color & 0x10) && (r[kBlendControl0] & 0x1FFF1FFF) == 0x00010001 &&
           (!(depth & 2) || always(depth >> 4)) && (!(depth & 1) || (always(depth >> 8) && (!(depth & 0x80) || always(depth >> 20))));
}

// An EDRAM pixel rectangle, right and bottom exclusive.
struct PixelRectangle
{
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;

    bool operator==(const PixelRectangle&) const = default;
};

std::unique_ptr<rex::graphics::RegisterFile> registerFile(std::span<const std::uint32_t> words)
{
    UNSCOPED_INFO("a full register file");
    REQUIRE(words.size() == rex::graphics::RegisterFile::kRegisterCount);
    auto file = std::make_unique<rex::graphics::RegisterFile>();
    std::copy(words.begin(), words.end(), file->values);
    return file;
}

// Scissor applies window offset, screen clipping, optional pitch clamp and
// nonnegative bounds.
PixelRectangle scissorOf(const rex::graphics::RegisterFile& regs, bool clampToPitch)
{
    using namespace rex::graphics;
    const auto   windowTl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    const auto   windowBr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    std::int32_t left = std::int32_t(windowTl.tl_x), top = std::int32_t(windowTl.tl_y);
    std::int32_t right = std::int32_t(windowBr.br_x), bottom = std::int32_t(windowBr.br_y);
    if (!windowTl.window_offset_disable)
    {
        const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        left += offset.window_x_offset, top += offset.window_y_offset;
        right += offset.window_x_offset, bottom += offset.window_y_offset;
    }
    const auto screenTl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
    const auto screenBr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
    left = std::max(left, std::int32_t(screenTl.tl_x)), top = std::max(top, std::int32_t(screenTl.tl_y));
    right = std::min(right, std::int32_t(screenBr.br_x)), bottom = std::min(bottom, std::int32_t(screenBr.br_y));
    if (clampToPitch)
    {
        const auto pitch = std::int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch);
        left = std::min(left, pitch), right = std::min(right, pitch);
    }
    left = std::max(left, 0), top = std::max(top, 0);
    return { left, top, std::max(right, left), std::max(bottom, top) };
}

// Resolve footprint follows fetch vertices, half-pixel top-left rasterization,
// scissor, 8-pixel expansion and surface bounds.
PixelRectangle resolveFootprint(std::span<const std::uint32_t> words, std::uint64_t base, std::span<const std::uint8_t> vertices, const std::string& name)
{
    using namespace rex::graphics;
    const auto regs  = registerFile(words);
    const auto fetch = regs->GetVertexFetch(0);
    UNSCOPED_INFO(name + " reads its rectangle through vertex fetch 0");
    REQUIRE((fetch.type == xenos::FetchConstantType::kVertex && fetch.size == 6 && std::uint64_t(fetch.address) * 4 == base && vertices.size() == 24));
    const float  half = regs->Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5F : 0.0F;
    std::int32_t fixed[6];
    for (std::size_t i = 0; i < 6; ++i)
        fixed[i] = rex::ui::FloatToD3D11Fixed16p8(xenos::GpuSwap(std::bit_cast<float>(little(vertices, i * 4)), fetch.endian) + half);
    std::int32_t x0 = (std::min({ fixed[0], fixed[2], fixed[4] }) + 127) >> 8, y0 = (std::min({ fixed[1], fixed[3], fixed[5] }) + 127) >> 8;
    std::int32_t x1 = (std::max({ fixed[0], fixed[2], fixed[4] }) + 127) >> 8, y1 = (std::max({ fixed[1], fixed[3], fixed[5] }) + 127) >> 8;
    if (regs->Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
    {
        const auto offset = regs->Get<reg::PA_SC_WINDOW_OFFSET>();
        x0 += offset.window_x_offset, x1 += offset.window_x_offset;
        y0 += offset.window_y_offset, y1 += offset.window_y_offset;
    }
    const auto scissor = scissorOf(*regs, false);
    x0 = std::clamp(x0, scissor.left, scissor.right), x1 = std::clamp(x1, scissor.left, scissor.right);
    y0 = std::clamp(y0, scissor.top, scissor.bottom), y1 = std::clamp(y1, scissor.top, scissor.bottom);
    constexpr auto alignment = std::int32_t(xenos::kResolveAlignmentPixels);
    x0 &= ~(alignment - 1), y0 &= ~(alignment - 1);
    x1 = (x1 + alignment - 1) & ~(alignment - 1), y1 = (y1 + alignment - 1) & ~(alignment - 1);
    const auto surface = regs->Get<reg::RB_SURFACE_INFO>();
    UNSCOPED_INFO(name + " resolves at most 4x MSAA");
    REQUIRE(surface.msaa_samples <= xenos::MsaaSamples::k4X);
    const auto pitch = std::int32_t(surface.surface_pitch & ~std::uint32_t(alignment - 1));
    if (x1 > pitch)
        x0 = std::min(x0, pitch), x1 = std::min(x1, pitch);
    y1 = std::min(y1, y0 + std::int32_t(xenos::kMaxResolveSize));
    UNSCOPED_INFO(name + " resolves a nonempty rectangle");
    REQUIRE((x0 < x1 && y0 < y1));
    return { x0, y0, x1, y1 };
}

// Guest memory as a set of captured regions. A read inside one region returns
// its bytes; any other read returns nothing, as an unmapped guest range does.
struct FakeMemory
{
    struct Region
    {
        std::uint64_t             address  = 0;
        bool                      physical = false;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Region> regions;
    unsigned            reads = 0;

    void Add(std::uint64_t address, bool physical, std::vector<std::uint8_t> bytes)
    {
        regions.push_back({ address, physical, std::move(bytes) });
    }

    Region& At(std::uint64_t address, bool physical)
    {
        for (auto& region : regions)
            if (region.address == address && region.physical == physical)
                return region;
        throw std::runtime_error("no captured region at the mutated address");
    }

    std::vector<std::uint8_t> Read(std::uint64_t address, std::uint64_t size, bool physical)
    {
        ++reads;
        for (const auto& region : regions)
            if (region.physical == physical && address >= region.address && address - region.address + size <= region.bytes.size())
                return { region.bytes.begin() + (address - region.address), region.bytes.begin() + (address - region.address + size) };
        return {};
    }
};

ReadMemory readerFor(FakeMemory& memory)
{
    return testing::MirrorFixtureReads(ReadMemory([&memory](std::uint64_t address, std::uint64_t size, bool physical)
                                                  {
                                                      return memory.Read(address, size, physical);
                                                  }));
}

NativeDrawSubmission movieSubmission(const std::array<std::uint32_t, 8>& arguments, std::uint32_t caller = kMovieDrawCaller, NativeDrawHook hook = NativeDrawHook::NonindexedUp)
{
    NativeDrawSubmission submission;
    submission.hook      = hook;
    submission.caller    = caller;
    submission.stack     = 0x70000000;
    submission.arguments = arguments;
    return submission;
}

// The S11 luma and first chroma fetches of event 5490 (G+0x480, G+0x498):
// linear k_8, clamp to edge, linear filters, mip filter base map, one level.
constexpr std::array<std::uint32_t, 6> kLumaFetch{ 0x0A024802, 0x1ADBD002, 0x0059E4FF, 0x01280248, 0x00000003, 0x00000218 };
constexpr std::array<std::uint32_t, 6> kChromaFetch{ 0x06024802, 0x1AD79002, 0x002CE27F, 0x01280248, 0x00000003, 0x00000218 };

void unit()
{
    std::string                        error;
    FakeMemory                         memory;
    const auto                         read = readerFor(memory);
    const std::array<std::uint32_t, 8> tuple{ 0x50000000, 6, 4, 0x50002000, 20, 0, 0, 0 };
    MovieDrawInputs                    inputs;

    // Selection happens before any read: hook, caller, then stride.
    UNSCOPED_INFO("another hook is foreign without reads");
    REQUIRE((!ReadMovieDrawInputs(movieSubmission(tuple, kMovieDrawCaller, NativeDrawHook::IndexedUp), read, inputs, error) && error.empty() &&
             memory.reads == 0));
    UNSCOPED_INFO("the call instruction's own address is not the return site");
    REQUIRE((!ReadMovieDrawInputs(movieSubmission(tuple, 0x82E39878), read, inputs, error) && error.empty() && memory.reads == 0));
    auto stride16 = tuple;
    stride16[4]   = 16;
    UNSCOPED_INFO("stride 16 is foreign without reads");
    REQUIRE((!ReadMovieDrawInputs(movieSubmission(stride16), read, inputs, error) && error.empty() && memory.reads == 0 && !inputs.movieCandidate));
    UNSCOPED_INFO("a movie-site call with unreadable state is named: " + error);
    REQUIRE((!ReadMovieDrawInputs(movieSubmission(tuple), read, inputs, error) && error == "movie_shadow_extent"));

    // A pixel shader other than the pinned one is foreign after its read.
    std::vector<std::uint8_t> state(0x3500), header(68), metadata(32), code(96, 0x5A);
    store(state, 0x3194, 0x40000000);
    store(state, 0x3198, 0x40001000);
    store(header, 24, 0x1000);
    store(header, 64, 100);
    store(metadata, 4, static_cast<std::uint32_t>(code.size()));
    memory.Add(0x50000000, false, state);
    memory.Add(0x40000000, false, header);
    memory.Add(0x40000000 + 140, false, metadata);
    memory.Add(0x1000, true, code);
    UNSCOPED_INFO("another pixel shader is foreign: " + error);
    REQUIRE((!ReadMovieDrawInputs(movieSubmission(tuple), read, inputs, error) && error.empty() && !inputs.movieCandidate));
    UNSCOPED_INFO("only the pinned word digest is the movie PS");
    REQUIRE((!IsMoviePixelShader({}) && !IsMoviePixelShader(code)));
    {
        MovieDrawRecipe        recipe;
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO("the adapter leaves inputs with another pixel shader foreign: " + error);
        REQUIRE((!BuildMovieDrawRecipe(inputs, NativeGameplayShaderInterface{}, recipe, error) && error.empty()));
        UNSCOPED_INFO("a non-candidate builds nothing");
        REQUIRE((!BuildMovieDraw(inputs, NativeGameplayShaderInterface{}, draw, detail) && detail.empty()));
    }

    // Each plane's range is its pitched base level: 1280 * 720 luma bytes,
    // and 768 * 359 + 640 bytes per chroma plane, 1,474,304 bytes per draw.
    NativeTextureMemoryRange luma, chroma;
    UNSCOPED_INFO("the luma range: " + error);
    REQUIRE((GetNativeTextureMemoryRange(kLumaFetch, luma, error) && luma.address == 0x1ADBD000 && luma.size == 921600));
    UNSCOPED_INFO("the chroma range: " + error);
    REQUIRE((GetNativeTextureMemoryRange(kChromaFetch, chroma, error) && chroma.address == 0x1AD79000 && chroma.size == 276352));
    UNSCOPED_INFO("plane bytes per draw");
    REQUIRE(luma.size + 2 * chroma.size == 1474304);

    // The planes' sampler: clamp to edge and linear filters, and mip filter
    // base map on one level, which is no mip filter.
    for (const auto& fetch : { kLumaFetch, kChromaFetch })
    {
        NativeDrawReplaySampler sampler;
        UNSCOPED_INFO("the plane sampler: " + error);
        REQUIRE((DecodeMovieSampler(fetch, sampler, error) && error.empty() && sampler.magLinear && sampler.minLinear && !sampler.mipLinear &&
                 sampler.address == std::array<std::uint32_t, 3>{ 3, 3, 3 } && sampler.maxLod == std::numeric_limits<float>::max()));
    }
    const auto with = [](std::size_t index, std::uint32_t value)
    {
        auto words   = kLumaFetch;
        words[index] = value;
        return words;
    };
    const auto refusal = [&](const char* name, const std::array<std::uint32_t, 6>& words, const char* expected)
    {
        NativeDrawReplaySampler refused;
        std::string             reason;
        UNSCOPED_INFO(std::string(name) + ": " + reason);
        REQUIRE((!DecodeMovieSampler(words, refused, reason) && reason == expected));
    };
    refusal("mip filter 3", with(3, kLumaFetch[3] | (3U << 23)), "movie_sampler_unsupported");
    refusal("a mip chain", with(4, kLumaFetch[4] | (1U << 6)), "movie_texture_mipmapped");
    refusal("a texture border", with(3, kLumaFetch[3] | 0x80000000U), "movie_sampler_border_size");
    refusal("the OpenGL nearest clamp policy", with(1, kLumaFetch[1] | 0x800U), "movie_sampler_nearest_clamp_policy");
    refusal("a halfway clamp", with(0, (kLumaFetch[0] & ~(7U << 10)) | (4U << 10)), "movie_sampler_clamp_mode");
    auto biased = with(3, (kLumaFetch[3] & ~(3U << 21)));
    biased[4]   = 0x00080003;
    refusal("point minification, linear magnification and a LOD bias", biased, "movie_sampler_lod_bias");
    auto gradient = with(3, (kLumaFetch[3] & ~(3U << 21)));
    gradient[4]   = 0x08000003;
    refusal("unequal filters and a gradient adjust", gradient, "movie_sampler_gradient_adjust");

    // Composition: the movie family is tried after panel, label and GFx, and
    // their foreign verdicts pass the movie quad on.
    memory.reads                  = 0;
    const auto             shader = std::make_shared<NativeGameplayShaderInterface>();
    NativeDrawReplayRecipe draw;
    std::string            detail;
    auto                   none = MakeBootDrawBuilder(read, nullptr, shader, std::make_shared<GfxDrawCatalog>());
    UNSCOPED_INFO("without a movie shader the quad has no route, and label and GFx read nothing");
    REQUIRE((!none(movieSubmission(tuple), draw, detail) && detail.empty() && memory.reads == 0));
    auto all = MakeBootDrawBuilder(read, nullptr, shader, std::make_shared<GfxDrawCatalog>(), nullptr, shader);
    UNSCOPED_INFO("label and GFx pass the quad on and the movie reader finds a foreign PS");
    REQUIRE((!all(movieSubmission(tuple), draw, detail) && detail.empty() && memory.reads > 0));
    memory.reads    = 0;
    auto movieOnly  = MakeBootDrawBuilder(read, nullptr, nullptr, nullptr, nullptr, shader);
    auto labelTuple = tuple;
    labelTuple[4]   = 28;
    UNSCOPED_INFO("the movie reader leaves a label batch alone");
    REQUIRE((!movieOnly(movieSubmission(labelTuple, kLabelDrawCaller), draw, detail) && detail.empty() && memory.reads == 0));
    // Control flow "exec 1; alloc <type>" then one instruction slot. Only the
    // memory alloc exports; words after the lowest exec address are not
    // control flow even when they decode as a memory alloc.
    const auto program = [](std::uint32_t allocWord)
    {
        return std::vector<std::uint32_t>{ 0x00001001, 0x00001000, allocWord, 0, 0, 0xC6000000 };
    };
    UNSCOPED_INFO("only a control-flow memory alloc exports");
    REQUIRE((exportsMemory(program(0xC6000000)) && !exportsMemory(program(0xC2000000)) && !exportsMemory(program(0xC4000000))));
    // "exec <count>" with the given sequence bits over one ALU slot whose
    // first word sets the export flag (bit 15), masks and destination.
    const auto clause = [](std::uint32_t exec, std::uint32_t alu)
    {
        return std::vector<std::uint32_t>{ exec, 0x00001000, 0, alu, 0, 0 };
    };
    UNSCOPED_INFO("ALU exports to eA and eM0-eM4 export, other exports, temporaries and fetch slots do not, and clauses past the microcode do");
    REQUIRE((exportsMemory(clause(0x00001001, 0x000F8020)) && exportsMemory(clause(0x00001001, 0x000F8025)) &&
             !exportsMemory(clause(0x00001001, 0x000F803E)) && !exportsMemory(clause(0x00001001, 0x000F8026)) &&
             !exportsMemory(clause(0x00001001, 0x000F0020)) && !exportsMemory(clause(0x00011001, 0x000F8020)) &&
             exportsMemory(clause(0x00002001, 0))));

    std::cout << "Movie reader selection, plane ranges, sampler, builder composition and export decoding checks passed\n";
}

// ---------------------------------------------------------------------------
// The S11 movie capture: 16 frames, each one DrawPrimitiveUP root from
// 0x82E3987C with its nested NonindexedSink child folded into it.

struct Binding
{
    std::filesystem::path file;
    std::uint64_t         address  = 0;
    bool                  physical = false;
    std::string           sha256;
};

// A captured resolve: the guest Resolve's rectangle, destination point and
// flags, and its destination texture's physical base and fetch words.
struct CapturedResolve
{
    std::uint64_t                sequence = 0;
    std::array<std::uint32_t, 4> rectangle{};
    std::array<std::uint32_t, 2> point{};
    std::uint32_t                flags = 0, pitch = 0, width = 0, height = 0, format = 0, base = 0;
    std::array<std::uint32_t, 6> fetch{};
    // RB_COPY_DEST_BASE and RB_COPY_DEST_PITCH words G+0x2A1C and G+0x2A20 at
    // entry, which the previous resolve left; its control and info words are
    // zero again by then.
    std::optional<std::array<std::uint32_t, 2>> previousCopy;
};

struct Event
{
    std::uint64_t                       sequence = 0, frame = 0;
    std::array<std::uint32_t, 8>        arguments{};
    std::multimap<std::string, Binding> bindings;
    // The frame's resolves after the draw, and the present that ends it.
    std::vector<CapturedResolve> resolves;
    std::uint64_t                presentSequence = 0;
    std::uint32_t                frontbuffer     = 0;
    std::array<std::uint32_t, 6> presentFetch{};

    const Binding& one(const std::string& role) const
    {
        UNSCOPED_INFO("event " + std::to_string(sequence) + " has one " + role);
        REQUIRE(bindings.count(role) == 1);
        return bindings.find(role)->second;
    }
};

std::vector<Event> loadRoots(const std::filesystem::path& capture)
{
    const auto  table = toml::parse_file((capture / "events.toml").string());
    const auto* all   = table["events"].as_array();
    UNSCOPED_INFO("capture events");
    REQUIRE(all != nullptr);
    // A hook record's destination or presented texture.
    const auto texture = [](const toml::table& event, std::uint32_t& base, std::array<std::uint32_t, 6>& fetch)
    {
        const auto* words = event["texture"]["words"].as_array();
        UNSCOPED_INFO("a texture's fetch words");
        REQUIRE((words && words->size() == 6));
        base = std::uint32_t(event["texture"]["base"].value_or<std::int64_t>(0));
        for (std::size_t i = 0; i < 6; ++i)
            fetch[i] = std::uint32_t((*words)[i].value_or<std::int64_t>(0));
    };

    std::vector<Event>                     roots;
    std::map<std::uint64_t, std::uint64_t> children; // parent -> child
    // By frame: resolves, and presents as (sequence, base, fetch).
    std::map<std::uint64_t, std::vector<CapturedResolve>>                                                        resolves;
    std::map<std::uint64_t, std::vector<std::tuple<std::uint64_t, std::uint32_t, std::array<std::uint32_t, 6>>>> presents;
    for (const auto& node : *all)
    {
        const auto& event = *node.as_table();
        const auto  kind  = event["kind"].value_or(std::string());
        const auto  frame = std::uint64_t(event["frame"].value_or<std::int64_t>(0));
        if (kind == "resolve")
        {
            CapturedResolve resolve;
            resolve.sequence  = std::uint64_t(event["sequence"].value_or<std::int64_t>(0));
            const auto* rect  = event["rectangle"].as_array();
            const auto* point = event["destination_offset_xy"].as_array();
            UNSCOPED_INFO("a resolve's rectangle and point");
            REQUIRE((rect && rect->size() == 4 && point && point->size() == 2));
            for (std::size_t i = 0; i < 4; ++i)
                resolve.rectangle[i] = std::uint32_t((*rect)[i].value_or<std::int64_t>(-1));
            for (std::size_t i = 0; i < 2; ++i)
                resolve.point[i] = std::uint32_t((*point)[i].value_or<std::int64_t>(-1));
            resolve.flags  = std::uint32_t(event["flags"].value_or<std::int64_t>(-1));
            resolve.pitch  = std::uint32_t(event["raw_resolve_pitch"].value_or<std::int64_t>(0));
            resolve.width  = std::uint32_t(event["resolve_width"].value_or<std::int64_t>(0));
            resolve.height = std::uint32_t(event["resolve_height"].value_or<std::int64_t>(0));
            resolve.format = std::uint32_t(event["destination_format"].value_or<std::int64_t>(0));
            texture(event, resolve.base, resolve.fetch);
            resolves[frame].push_back(resolve);
            continue;
        }
        if (kind == "present")
        {
            std::uint32_t                base = 0;
            std::array<std::uint32_t, 6> fetch{};
            texture(event, base, fetch);
            presents[frame].emplace_back(std::uint64_t(event["sequence"].value_or<std::int64_t>(0)), base, fetch);
            continue;
        }
        if (kind != "draw")
            continue;
        const auto sequence = std::uint64_t(event["sequence"].value_or<std::int64_t>(0));
        const auto entry    = event["entry"].value_or<std::int64_t>(0);
        const auto caller   = std::stoull(event["caller_lr"].value_or(std::string("0")));
        const auto parent   = std::uint64_t(event["parent_call_id"].value_or<std::int64_t>(0));
        if (entry == 0x826A2B58)
        {
            // The nested NonindexedSink record: the same submission, folded.
            UNSCOPED_INFO("a sink child returns into its DrawPrimitiveUP root");
            REQUIRE((caller == 0x826A3024 && parent && !children.contains(parent)));
            children[parent] = sequence;
            continue;
        }
        UNSCOPED_INFO("every root is DrawPrimitiveUP from the movie site");
        REQUIRE((entry == 0x826A3000 && caller == kMovieDrawCaller && parent == 0));
        Event root;
        root.sequence    = sequence;
        root.frame       = std::uint64_t(event["frame"].value_or<std::int64_t>(0));
        const auto* args = event["raw_r3_r10_u64"].as_array();
        UNSCOPED_INFO("root raw arguments");
        REQUIRE((args && args->size() == 8));
        for (std::size_t i = 0; i < 8; ++i)
            root.arguments[i] = static_cast<std::uint32_t>(std::stoull((*args)[i].value_or(std::string("0"))));
        roots.push_back(std::move(root));
    }
    UNSCOPED_INFO("the capture holds 16 roots, each with one folded sink child");
    REQUIRE((roots.size() == 16 && children.size() == 16));
    for (auto& root : roots)
    {
        UNSCOPED_INFO("each root has its child");
        REQUIRE(children.contains(root.sequence));
        const auto frame = presents.find(root.frame);
        UNSCOPED_INFO("frame " + std::to_string(root.frame) + " presents once after its movie draw");
        REQUIRE((frame != presents.end() && frame->second.size() == 1 && std::get<0>(frame->second[0]) > root.sequence));
        std::tie(root.presentSequence, root.frontbuffer, root.presentFetch) = frame->second[0];
        for (const auto& resolve : resolves[root.frame])
            if (resolve.sequence > root.sequence && resolve.sequence < root.presentSequence)
                root.resolves.push_back(resolve);
    }

    const auto  manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        const auto  event   = std::uint64_t(binding["event"].value_or<std::int64_t>(-1));
        if (binding["role"].value_or(std::string()) == "resolve_state_pre")
        {
            for (auto& root : roots)
                for (auto& resolve : root.resolves)
                    if (resolve.sequence == event)
                    {
                        const auto state = read(capture / binding["file"].value_or(std::string()));
                        UNSCOPED_INFO("resolve " + std::to_string(event) + " captures its device state");
                        REQUIRE((binding["address"].value_or<std::int64_t>(0) == root.arguments[0] && state.size() >= 0x2A24));
                        resolve.previousCopy = std::array<std::uint32_t, 2>{ word(state, 0x2A1C), word(state, 0x2A20) };
                    }
            continue;
        }
        const auto root = std::ranges::find(roots, event, &Event::sequence);
        if (root == roots.end())
            continue;
        UNSCOPED_INFO("payload error on event " + std::to_string(event));
        REQUIRE(binding["error"].value_or(std::string("x")).empty());
        root->bindings.emplace(binding["role"].value_or(std::string()),
                               Binding{ capture / binding["file"].value_or(std::string()), std::uint64_t(binding["address"].value_or<std::int64_t>(0)), binding["physical"].value_or(false), binding["sha256"].value_or(std::string()) });
    }
    return roots;
}

// The draw's state after Original: the captured pre-state plus its recorded
// (offset, before, after) word deltas.
std::vector<std::uint8_t> postState(const std::filesystem::path& capture, const Event& e)
{
    auto       state = read(capture / ("draw-" + std::to_string(e.sequence) + ".state.be.bin"));
    const auto delta = read(e.one("state_delta_offset_before_after_be32").file);
    UNSCOPED_INFO("state and delta extents");
    REQUIRE((state.size() == 0x3500 && delta.size() % 12 == 0));
    for (std::size_t i = 0; i < delta.size(); i += 12)
    {
        const auto offset = word(delta, i);
        UNSCOPED_INFO("delta before-word matches the pre-state");
        REQUIRE((offset + 4 <= state.size() && word(state, offset) == word(delta, i + 4)));
        store(state, offset, word(delta, i + 8));
    }
    return state;
}

// Guest memory at one root's entry: its retained payloads and its pre-state.
FakeMemory memoryAt(const std::filesystem::path& capture, const Event& e)
{
    FakeMemory memory;
    for (const auto& [role, binding] : e.bindings)
        if (!role.starts_with("state_"))
            memory.Add(binding.address, binding.physical, read(binding.file));
    memory.Add(e.arguments[0], false, read(capture / ("draw-" + std::to_string(e.sequence) + ".state.be.bin")));
    return memory;
}

std::vector<std::uint32_t> registersOf(std::span<const std::uint8_t> state)
{
    std::vector<std::uint32_t> registers(0x5003);
    for (const auto [from, to, count] : { std::array<unsigned, 3>{ 0x2880, 0x2000, 16 }, { 0x28C0, 0x2080, 3 }, { 0x28CC, 0x2100, 21 }, { 0x2920, 0x2180, 5 }, { 0x2934, 0x2200, 12 }, { 0x29B8, 0x2300, 38 }, { 0x780, 0x4000, 1024 }, { 0x1780, 0x4400, 896 }, { 0x2780, 0x4900, 40 } })
        for (unsigned i = 0; i < count; ++i)
            registers[to + i] = word(state, from + i * 4);
    return registers;
}

std::vector<std::uint32_t> words(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint32_t> result(bytes.size() / 4);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = word(bytes, i * 4);
    return result;
}

std::array<std::uint32_t, 6> fetchAt(std::span<const std::uint8_t> state, std::size_t slot)
{
    std::array<std::uint32_t, 6> fetch{};
    for (std::size_t i = 0; i < fetch.size(); ++i)
        fetch[i] = word(state, kMovieFetchShadows[slot] + i * 4);
    return fetch;
}

// Control replays captured outputs and shares no reader, folding or assembler
// code.
struct Control
{
    std::array<NativeDrawReplayTexture, 3> textures;
    std::array<NativeDrawReplaySampler, 3> samplers;
    std::vector<std::uint32_t>             registers, vs, ps;
    std::vector<std::uint8_t>              vertices;
};

void controlInputs(const std::filesystem::path& capture, const std::filesystem::path& catalog, const Event& e, Control& control)
{
    const auto state  = postState(capture, e);
    control.registers = registersOf(state);
    const auto values = read(e.one("ps_literal_values:508").file);
    UNSCOPED_INFO("the literal block");
    REQUIRE(values.size() == 64);
    for (std::uint32_t i = 0; i < 16; ++i)
        control.registers[0x47F0 + i] = word(values, i * 4);
    for (std::size_t slot = 0; slot < 3; ++slot)
    {
        const auto fetch = fetchAt(state, slot);
        Binding    plane;
        for (const auto& [role, binding] : e.bindings)
            if (role == "sampled_base" && binding.address == (fetch[1] & 0xFFFFF000U))
                plane = binding;
        UNSCOPED_INFO("a sampled_base payload for each plane");
        REQUIRE(!plane.file.empty());
        std::string error;
        UNSCOPED_INFO("control plane: " + error);
        REQUIRE(DecodeNativeTexture(fetch, read(plane.file), control.textures[slot], error));
        auto& sampler     = control.samplers[slot];
        sampler.address   = { 3, 3, 3 };
        sampler.magLinear = sampler.minLinear = true;
        sampler.maxLod                        = std::numeric_limits<float>::max();
    }
    control.vertices          = read(e.one("vertices").file);
    const auto base           = (e.arguments[3] & 0x1FFFFFFFU) + (((e.arguments[3] >> 20) + 512) & 0x1000);
    control.registers[0x48BE] = base | 3;
    control.registers[0x48BF] = static_cast<std::uint32_t>(control.vertices.size()) | 0x10000002U;
    control.vs                = words(read(catalog / "vs.ucode.be.bin"));
    control.ps                = words(read(e.one("ps_code").file));
}

bool controlRecipe(Control& control, const NativeGameplayShaderInterface& shader, NativeDrawReplayRecipe& recipe, std::string& error)
{
    const auto                                         base = control.registers[0x48BE] & ~3U;
    const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, control.vertices } } };
    const std::array<NativeGameplayTextureBinding, 3>  bindings{ {
        { 0, &control.textures[0], &control.samplers[0] },
        { 1, &control.textures[1], &control.samplers[1] },
        { 2, &control.textures[2], &control.samplers[2] },
    } };
    NativeGameplayDrawView                             view;
    view.registers       = control.registers;
    view.vertexMicrocode = control.vs;
    view.pixelMicrocode  = control.ps;
    view.vertexFetches   = fetches;
    view.textureBindings = bindings;
    view.primitive       = 6;
    view.indexCount      = 4;
    view.indexed         = false;
    NativeGameplayDrawTarget target;
    target.fullViewportTarget = true;
    target.width              = 1280;
    target.height             = 720;
    target.viewport           = { 0, 0, 1280, 720, 0, 1 };
    target.scissor            = { 0, 0, 1280, 720 };
    return BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error);
}

std::array<std::uint32_t, 256> gammaRamp()
{
    std::array<std::uint32_t, 256> ramp{};
    for (std::uint32_t i = 0; i < ramp.size(); ++i)
    {
        const auto value = (i * 1023U + 127U) / 255U;
        ramp[i]          = (value << 20) | (value << 10) | value;
    }
    return ramp;
}

// Movie reference decodes luma/chroma PS constants into RGB10 with bilinear
// RGBA8 and gamma ramp.
std::vector<std::array<std::uint16_t, 3>> referenceFrame(const std::filesystem::path& capture, const Event& e)
{
    const auto state  = postState(capture, e);
    const auto values = read(e.one("ps_literal_values:508").file);
    const auto c      = [&](unsigned index, unsigned component)
    {
        return double(std::bit_cast<float>(word(values, (index - 252) * 16 + component * 4)));
    };
    std::array<double, 4> c0{};
    for (unsigned i = 0; i < 4; ++i)
        c0[i] = std::bit_cast<float>(word(state, 0x1780 + i * 4));

    struct Plane
    {
        std::vector<std::uint8_t> bytes;
        int                       width = 0, height = 0, pitch = 0;
    };

    std::array<Plane, 3> planes;
    for (std::size_t slot = 0; slot < 3; ++slot)
    {
        const auto fetch = fetchAt(state, slot);
        for (const auto& [role, binding] : e.bindings)
            if (role == "sampled_base" && binding.address == (fetch[1] & 0xFFFFF000U))
                planes[slot].bytes = read(binding.file);
        planes[slot].width  = int(fetch[2] & 0x1FFF) + 1;
        planes[slot].height = int((fetch[2] >> 13) & 0x1FFF) + 1;
        planes[slot].pitch  = int((fetch[0] >> 22) & 0x1FF) * 32;
    }
    const auto sample = [](const Plane& plane, double u, double v)
    {
        const double tx = u * plane.width - 0.5, ty = v * plane.height - 0.5;
        const int    x0 = int(std::floor(tx)), y0 = int(std::floor(ty));
        const double fx = tx - x0, fy = ty - y0;
        const auto   texel = [&](int x, int y)
        {
            x = std::clamp(x, 0, plane.width - 1);
            y = std::clamp(y, 0, plane.height - 1);
            return plane.bytes[std::size_t(y) * plane.pitch + x] / 255.0;
        };
        return (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) + (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
    };
    const auto                                ramp = gammaRamp();
    std::vector<std::array<std::uint16_t, 3>> pixels(1280 * 720);
    for (int y = 0; y < 720; ++y)
        for (int x = 0; x < 1280; ++x)
        {
            const double                u = x / 1280.0, v = y / 720.0;
            const double                s0 = sample(planes[0], u, v), s1 = sample(planes[1], u, v), s2 = sample(planes[2], u, v);
            const std::array<double, 3> rgb{ s0 * c(253, 2) + s1 * c(253, 3) + c(255, 0),
                                             s0 * c(254, 2) + s1 * c(254, 3) + s2 * c(253, 0) + c(254, 0),
                                             s0 * c(254, 2) + s2 * c(253, 1) + c(254, 1) };
            // 4X samples 0 and 3 from the guest's 2X pair; edge pixels retain
            // sample 0.
            const auto covered = [&](double dx, double dy)
            {
                const double sx = x + 0.5 + dx, sy = y + 0.5 + dy;
                return sx >= 0.5 && sx < 1280.5 && sy >= 0.5 && sy < 720.5;
            };
            const bool first = covered(-2 / 16.0, -6 / 16.0), last = covered(2 / 16.0, 6 / 16.0);
            for (unsigned i = 0; i < 3; ++i)
            {
                const auto unorm                     = static_cast<std::uint32_t>(std::lround(std::clamp(rgb[i] * c0[i], 0.0, 1.0) * 255.0));
                const auto index                     = ((first ? unorm : 0) + (last ? unorm : 0) + 1) >> 1;
                pixels[std::size_t(y) * 1280 + x][i] = static_cast<std::uint16_t>((ramp[index] >> (20 - i * 10)) & 1023);
            }
        }
    return pixels;
}

struct Comparison
{
    unsigned    maximum = 0;
    std::size_t over    = 0;
    // Bounds of the pixels above the bound, and the worst pixel.
    std::size_t             left = SIZE_MAX, top = SIZE_MAX, right = 0, bottom = 0, worst = 0;
    std::array<unsigned, 3> actual{}, expected{};
};

Comparison compare(const std::vector<std::uint8_t>& output, const std::vector<std::array<std::uint16_t, 3>>& reference, unsigned bound)
{
    UNSCOPED_INFO("a 1280x720 RGB10 output");
    REQUIRE(output.size() == reference.size() * 4);
    Comparison result;
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        const auto packed = little(output, i * 4);
        // R10G10B10A2_UNORM: red in the low bits.
        const std::array<unsigned, 3> rgb{ packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 1023 };
        unsigned                      worst = 0;
        for (unsigned c = 0; c < 3; ++c)
            worst = std::max(worst, unsigned(std::abs(int(rgb[c]) - int(reference[i][c]))));
        if (worst > result.maximum)
        {
            result.maximum  = worst;
            result.worst    = i;
            result.actual   = rgb;
            result.expected = { reference[i][0], reference[i][1], reference[i][2] };
        }
        if (worst > bound)
        {
            ++result.over;
            result.left   = std::min(result.left, i % 1280);
            result.right  = std::max(result.right, i % 1280);
            result.top    = std::min(result.top, i / 1280);
            result.bottom = std::max(result.bottom, i / 1280);
        }
    }
    return result;
}

// A copy of the catalog with one shaders.toml edit, loaded through the movie
// loader; the expected refusal names the broken rule.
void catalogRefusal(const std::filesystem::path& catalog, const char* name, const std::function<void(std::string&)>& edit, const std::string& expected)
{
    const auto copy = std::filesystem::temp_directory_path() / ("movie-catalog-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::hash<std::string>{}(name)));
    std::filesystem::remove_all(copy);
    std::filesystem::copy(catalog, copy, std::filesystem::copy_options::recursive);
    std::string text;
    {
        std::ifstream in(copy / "shaders.toml", std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    edit(text);
    {
        std::ofstream out(copy / "shaders.toml", std::ios::binary | std::ios::trunc);
        out << text;
    }
    NativeGameplayShaderInterface rejected;
    std::string                   error;
    const bool                    loaded = LoadMovieDrawShader(copy, rejected, error);
    std::filesystem::remove_all(copy);
    UNSCOPED_INFO(std::string(name) + ": " + (loaded ? "accepted" : error));
    REQUIRE((!loaded && error == expected));
}

void replace(std::string& text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    UNSCOPED_INFO("catalog text has " + from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
}

// Reference copies record RB_COPY_CONTROL/DEST_INFO, depth clear and color
// clear in event order.
struct ReferenceCopy
{
    std::uint32_t control = 0, info = 0, depthClear = 0, colorClear = 0;
};

std::vector<ReferenceCopy> referenceCopyWords(const std::filesystem::path& directory)
{
    const auto  input    = directory / "input";
    const auto  manifest = toml::parse_file((input / "manifest.toml").string());
    const auto* events   = manifest["events"].as_array();
    UNSCOPED_INFO("Xenos reference events");
    REQUIRE(events != nullptr);
    std::vector<ReferenceCopy> copies;
    for (const auto& node : *events)
    {
        const auto* event = node.as_table();
        UNSCOPED_INFO("event rows");
        REQUIRE(event != nullptr);
        if ((*event)["kind"].value_or<std::int64_t>(-1) != 1)
            continue;
        const auto registers = read(input / (*event)["registers"]["file"].value_or(std::string()));
        UNSCOPED_INFO("a full register file");
        REQUIRE(registers.size() == 0x5003 * 4);
        copies.push_back({ little(registers, kCopyControl * 4), little(registers, kCopyDestInfo * 4), little(registers, kDepthClear * 4), little(registers, kColorClear * 4) });
    }
    return copies;
}

int s11(const std::filesystem::path& capture, const std::filesystem::path& catalog)
{
    std::string                   error;
    NativeGameplayShaderInterface shader;
    UNSCOPED_INFO("movie catalog: " + error);
    REQUIRE(LoadMovieDrawShader(catalog, shader, error));
    UNSCOPED_INFO("the catalog's screen-space movie pair");
    REQUIRE((shader.screenSpacePosition && shader.textureFetchSlots == std::vector<std::uint32_t>{ 0, 1, 2 }));

    // A second movie specialization after the valid one: the loader admits the
    // distinct identity, so only the one-entry rule refuses it.
    catalogRefusal(catalog, "a second movie specialization", [](std::string& t)
                   {
                       const auto at = t.find("[[shaders]]");
                       UNSCOPED_INFO("catalog text has an entry");
                       REQUIRE(at != std::string::npos);
                       auto second = t.substr(at);
                       replace(second, "screen_space_position = true", "screen_space_position = false");
                       t += "\n" + second;
                   },
                   "movie_catalog_entry");
    catalogRefusal(catalog, "another vertex hash", [](std::string& t)
                   {
                       replace(t, "C33871A8CFA8967A", "C33871A8CFA8967B");
                   },
                   "movie_catalog_entry");
    catalogRefusal(catalog, "another pixel word digest", [](std::string& t)
                   {
                       replace(t, std::string(kMoviePixelWordDigest), std::string(kMoviePixelWordDigest).replace(0, 1, "4"));
                   },
                   "movie_catalog_word_digest");
    catalogRefusal(catalog, "no screen-space specialization", [](std::string& t)
                   {
                       replace(t, "screen_space_position = true", "screen_space_position = false");
                   },
                   "movie_catalog_interface");
    catalogRefusal(catalog, "two texture fetches", [](std::string& t)
                   {
                       replace(t, "texture_fetches = [0, 1, 2]\ntexture_dimensions = [\"2d\", \"2d\", \"2d\"]", "texture_fetches = [0, 1]\ntexture_dimensions = [\"2d\", \"2d\"]");
                   },
                   "movie_catalog_interface");
    catalogRefusal(catalog, "an unsigned position", [](std::string& t)
                   {
                       replace(t, "signed = true", "signed = false");
                   },
                   "movie_catalog_layout");
    catalogRefusal(catalog, "a UV at offset 8", [](std::string& t)
                   {
                       replace(t, "offset_bytes = 12", "offset_bytes = 8");
                   },
                   "movie_catalog_layout");
    // One edit per remaining interface and layout field.
    const std::array<std::array<std::string, 4>, 12> edits{ {
        { "another vertex word digest", std::string(kMovieVertexWordDigest), std::string(kMovieVertexWordDigest).replace(0, 1, "f"), "movie_catalog_word_digest" },
        { "a cube chroma plane", "texture_dimensions = [\"2d\", \"2d\", \"2d\"]", "texture_dimensions = [\"2d\", \"2d\", \"cube\"]", "movie_catalog_interface" },
        { "an alpha test", "alpha_test = false", "alpha_test = true", "movie_catalog_interface" },
        { "another program", "program = 268501249", "program = 268501248", "movie_catalog_interface" },
        { "another context", "context = 4", "context = 8", "movie_catalog_interface" },
        { "other interpolators", "interpolators = 65537", "interpolators = 65539", "movie_catalog_interface" },
        { "texture fetches 0, 2, 1", "texture_fetches = [0, 1, 2]", "texture_fetches = [0, 2, 1]", "movie_catalog_interface" },
        { "a third attribute", "[[shaders.attributes]]", "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 20\noffset_bytes = 8\nformat = 37\nsigned = true\nnormalized = false\n\n[[shaders.attributes]]", "movie_catalog_interface" },
        { "fetch constant 94", "fetch = 95", "fetch = 94", "movie_catalog_layout" },
        { "a 24-byte stride", "stride_bytes = 20", "stride_bytes = 24", "movie_catalog_layout" },
        { "a k_32_32_32_32_FLOAT position", "format = 57", "format = 38", "movie_catalog_layout" },
        { "a normalized position", "normalized = false", "normalized = true", "movie_catalog_layout" },
    } };
    for (const auto& [name, from, to, expected] : edits)
        catalogRefusal(catalog, name.c_str(), [&](std::string& t)
                       {
                           replace(t, from, to);
                       },
                       expected);
    std::cout << "Movie catalog loads; entry, digest, interface and layout refusals are named\n";

    // Every root reads to the captured bytes and builds. Frames alternate
    // between two plane sets, each retained once per resource version.
    const auto                           roots = loadRoots(capture);
    std::map<std::uint32_t, std::string> lumaDigests;
    MovieTextureCache                    cache;
    for (const auto& root : roots)
    {
        auto            memory = memoryAt(capture, root);
        MovieDrawInputs inputs;
        UNSCOPED_INFO("movie inputs " + std::to_string(root.sequence) + ": " + error);
        REQUIRE((ReadMovieDrawInputs(movieSubmission(root.arguments), readerFor(memory), inputs, error) && inputs.movieCandidate && inputs.inputsComplete));
        UNSCOPED_INFO("reader inputs equal the S11 shader and vertex payloads");
        REQUIRE((inputs.vertexCode == read(root.one("vs_code").file) && inputs.pixelCode == read(root.one("ps_code").file) &&
                 inputs.vertices == read(root.one("vertices").file)));
        const bool                         setA     = root.frame % 2 == 0;
        const std::array<std::uint32_t, 3> expected = setA ? std::array<std::uint32_t, 3>{ 0x1ADBD000, 0x1AD79000, 0x1AD35000 }
                                                           : std::array<std::uint32_t, 3>{ 0x1AC54000, 0x1AC10000, 0x1ABCC000 };
        for (std::size_t slot = 0; slot < 3; ++slot)
        {
            const auto& plane = inputs.planes[slot];
            bool        found = false;
            for (const auto& [role, binding] : root.bindings)
                found |= role == "sampled_base" && binding.address == plane.address && binding.physical && read(binding.file) == plane.bytes;
            UNSCOPED_INFO("plane " + std::to_string(slot) + " of frame " + std::to_string(root.frame) + " is the S11 payload");
            REQUIRE((plane.address == expected[slot] && found));
        }
        for (const auto& [role, binding] : root.bindings)
            if (role == "sampled_base" && binding.address == inputs.planes[0].address)
                lumaDigests[binding.address] = binding.sha256;
        const auto values = read(root.one("ps_literal_values:508").file);
        UNSCOPED_INFO("sixteen literal registers");
        REQUIRE(inputs.literals.size() == 16);
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            UNSCOPED_INFO("literal registers equal the S11 block");
            REQUIRE(inputs.literals[i] == std::make_pair(0x47F0 + i, word(values, i * 4)));
        }

        MovieDrawRecipe recipe;
        UNSCOPED_INFO("movie recipe " + std::to_string(root.sequence) + ": " + error);
        REQUIRE(BuildMovieDrawRecipe(inputs, shader, recipe, error, &cache));
        UNSCOPED_INFO("the guest's pre-patch VS folds and patches to the catalog's words");
        REQUIRE((MovieMicrocodeWordDigest(inputs.vertexCode) != kMovieVertexWordDigest && MovieMicrocodeWordDigest(recipe.patchedVertexCode) == kMovieVertexWordDigest &&
                 recipe.patchedVertexCode == read(catalog / "vs.ucode.be.bin")));
        const auto post = postState(capture, root);
        UNSCOPED_INFO("effective registers and constants equal S11's Original state");
        REQUIRE(GuestDrawRegisters(recipe.effectiveState) == registersOf(post));
        const auto& constants = recipe.native.pixelConstants;
        UNSCOPED_INFO("all 256 pixel constants upload");
        REQUIRE(constants.size() == 256 * 16);
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            UNSCOPED_INFO("effective c252..c255 equal the literal block");
            REQUIRE(little(constants, 252 * 16 + i * 4) == word(values, i * 4));
        }
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            UNSCOPED_INFO("c0 equals the shadow");
            REQUIRE(little(constants, i * 4) == word(post, 0x1780 + i * 4));
        }
        UNSCOPED_INFO("catalog DXIL bound");
        REQUIRE((std::ranges::equal(recipe.native.vertexShaderBytes(), shader.vertexShaderDxil) && std::ranges::equal(recipe.native.pixelShaderBytes(), shader.pixelShaderDxil)));
        for (std::size_t slot = 0; slot < 3; ++slot)
        {
            UNSCOPED_INFO("plane textures and samplers");
            REQUIRE((recipe.textures[slot].width == (slot ? 640U : 1280U) && recipe.textures[slot].height == (slot ? 360U : 720U) &&
                     !recipe.samplers[slot].mipLinear && recipe.samplers[slot].magLinear));
        }
    }
    UNSCOPED_INFO("two plane sets alternate across the 16 frames");
    REQUIRE(lumaDigests.size() == 2);
    std::cout << "All 16 movie roots read to the S11 payloads and build; two plane sets ("
              << lumaDigests[0x1ADBD000].substr(0, 8) << ", " << lumaDigests[0x1AC54000].substr(0, 8) << ") alternate\n";

    // Each frame resolves two tiles to alternating frontbuffers at 1280x720,
    // then presents that buffer.
    const auto scanouts = [](const Event& root)
    {
        const auto frame = "frame " + std::to_string(root.frame);
        UNSCOPED_INFO(frame + " resolves two tiles");
        REQUIRE(root.resolves.size() == 2);
        UNSCOPED_INFO(frame + " presents its frontbuffer as ZYX1");
        REQUIRE(((root.presentFetch[1] & 0xFFFFF000U) == root.frontbuffer && root.presentFetch[3] == 0x1414));
        for (std::uint32_t tile = 0; tile < 2; ++tile)
        {
            const auto& resolve = root.resolves[tile];
            const auto  left    = tile * 640;
            UNSCOPED_INFO(frame + " tile " + std::to_string(tile) + " scans out its columns to the presented frontbuffer");
            REQUIRE((resolve.rectangle == std::array<std::uint32_t, 4>{ left, 0, left + 640, 720 } && resolve.point == std::array<std::uint32_t, 2>{ left, 0 } &&
                     resolve.flags == (tile ? 0U : 0x300U) && resolve.pitch == 1280 && resolve.width == 1280 && resolve.height == 720 &&
                     resolve.format == 6 && resolve.base == root.frontbuffer && resolve.fetch == root.presentFetch));
        }
    };
    for (std::size_t i = 0; i < roots.size(); ++i)
    {
        scanouts(roots[i]);
        UNSCOPED_INFO("frame " + std::to_string(roots[i].frame) + " presents the other frontbuffer");
        REQUIRE(((roots[i].frontbuffer == 0x1F6F8000 || roots[i].frontbuffer == 0x1F360000) && (!i || roots[i].frontbuffer != roots[i - 1].frontbuffer)));
    }
    std::cout << "All 16 movie frames scan out two tiles to their frontbuffer, alternating 1F6F8000 and 1F360000\n";

    // A held frame reuses its decoded planes; without the cache each build decodes.
    const auto& first = roots.front();
    UNSCOPED_INFO("the first root is frame 900");
    REQUIRE(first.frame == 900);
    {
        auto            memory = memoryAt(capture, first);
        MovieDrawInputs inputs;
        UNSCOPED_INFO(error);
        REQUIRE(ReadMovieDrawInputs(movieSubmission(first.arguments), readerFor(memory), inputs, error));
        MovieTextureCache held;
        MovieDrawRecipe   a, b, c;
        UNSCOPED_INFO(error);
        REQUIRE((BuildMovieDrawRecipe(inputs, shader, a, error, &held) && BuildMovieDrawRecipe(inputs, shader, b, error, &held) &&
                 BuildMovieDrawRecipe(inputs, shader, c, error)));
        for (std::size_t slot = 0; slot < 3; ++slot)
        {
            UNSCOPED_INFO("the per-slot cache keeps a held frame's planes");
            REQUIRE((a.textures[slot].textureVersion && a.textures[slot].textureVersion == b.textures[slot].textureVersion &&
                     c.textures[slot].textureVersion != a.textures[slot].textureVersion));
        }
    }

    // Reader refusals on frame 900's memory, one rule each.
    const auto readRefused = [&](const char* name, auto mutate, const std::string& expected)
    {
        auto memory = memoryAt(capture, first);
        auto tuple  = first.arguments;
        auto caller = kMovieDrawCaller;
        mutate(memory, tuple, caller);
        MovieDrawInputs inputs;
        std::string     reason;
        const bool      accepted = ReadMovieDrawInputs(movieSubmission(tuple, caller), readerFor(memory), inputs, reason);
        UNSCOPED_INFO(std::string(name) + ": " + (accepted ? "accepted" : "[" + reason + "]"));
        REQUIRE((!accepted && reason.starts_with(expected) && (expected.empty() ? reason.empty() && !inputs.movieCandidate : inputs.movieCandidate)));
    };
    using Tuple   = std::array<std::uint32_t, 8>;
    const auto G  = first.arguments[0];
    const auto at = [G](FakeMemory& memory, std::size_t offset, std::uint32_t value)
    {
        store(memory.At(G, false).bytes, offset, value);
    };
    const auto vsHeader = first.one("vs_header");
    readRefused("stride 16", [](FakeMemory&, Tuple& t, std::uint32_t&)
                {
                    t[4] = 16;
                },
                "");
    readRefused("the call instruction 0x82E39878 as caller", [](FakeMemory&, Tuple&, std::uint32_t& caller)
                {
                    caller = 0x82E39878;
                },
                "");
    readRefused("another pixel shader", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    m.At(0x1AEA0040, true).bytes[0] ^= 1;
                },
                "");
    readRefused("primitive 5", [](FakeMemory&, Tuple& t, std::uint32_t&)
                {
                    t[1] = 5;
                },
                "movie_unsupported_nonindexed_up_tuple");
    readRefused("six vertices", [](FakeMemory&, Tuple& t, std::uint32_t&)
                {
                    t[2] = 6;
                },
                "movie_unsupported_nonindexed_up_tuple");
    readRefused("a tiled chroma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x498, 0x06024802 | 0x80000000U);
                },
                "movie_plane_format");
    readRefused("a k_8_8 luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x484, 0x1ADBD000 | 10);
                },
                "movie_plane_format");
    // The texture decoder supports k_8_8_8_8, so only the k_8 pin refuses it.
    readRefused("a k_8_8_8_8 luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x484, 0x1ADBD000 | 6);
                },
                "movie_plane_format");
    readRefused("luma pitch 1024", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x480, (0x0A024802 & ~(0x1FFU << 22)) | (32U << 22));
                },
                "movie_plane_format");
    // A pitch wider than the plane is decodable, so only the pitch pin refuses it.
    readRefused("luma pitch 1536", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x480, (0x0A024802 & ~(0x1FFU << 22)) | (48U << 22));
                },
                "movie_plane_format");
    readRefused("a byte-swapped chroma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x4B4, 0x1AD35002 | (1U << 6));
                },
                "movie_plane_format");
    readRefused("a mipmapped luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x490, 3 | (1U << 6));
                },
                "movie_plane_format");
    readRefused("a 1280x736 luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x488, 0x0059E4FF + (16U << 13));
                },
                "movie_plane_format");
    readRefused("a 1264x720 luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x488, 0x0059E4FF - 16);
                },
                "movie_plane_format");
    readRefused("a 640x352 chroma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x4A0, 0x002CE27F - (8U << 13));
                },
                "movie_plane_format");
    readRefused("a stacked luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x484, 0x1ADBD002 | (1U << 10));
                },
                "movie_plane_format");
    readRefused("a 3D luma plane", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x494, (0x218 & ~(3U << 9)) | (2U << 9));
                },
                "movie_plane_format");
    readRefused("a vertex-fetch luma constant", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x480, 0x0A024802 | 3);
                },
                "movie_plane_format");
    readRefused("an unreadable chroma plane", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    m.At(0x1AD35000, true).bytes.resize(1000);
                },
                "movie_plane_read");
    readRefused("a second literal record", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    store(m.At(0x5144FCB8, false).bytes, 28, 0x01F80004);
                },
                "movie_literal_shape");
    readRefused("the literal record at c248", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    store(m.At(0x5144FCB8, false).bytes, 20, 0x01F80010);
                },
                "movie_literal_shape");
    readRefused("eight literal dwords", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    store(m.At(0x5144FCB8, false).bytes, 20, 0x01FC0008);
                },
                "movie_literal_shape");
    readRefused("a literal table past 4096 bytes", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    store(m.At(0x5144FCB8, false).bytes, 16, 8192);
                },
                "movie_literals: shader_literal_extent");
    readRefused("an unreadable literal block", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    m.At(0x1AEA0000, true).bytes.resize(32);
                },
                "movie_literals: shader_literal_unreadable");
    readRefused("a vertex literal record", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    // A one-record VS table at header + 0x100 loading c0.
                    auto& header = m.At(vsHeader.address, false).bytes;
                    store(header, 872 + 20, 0x100);
                    std::vector<std::uint8_t> table(28, 0);
                    store(table, 16, 8);
                    store(table, 20, 0x00000004);
                    m.Add(vsHeader.address + 872 + 0x100, false, table);
                    const auto raw = word(header, 32);
                    m.Add((raw & 0x1FFFFFFFU) + (((raw >> 20) + 512) & 0x1000), true, std::vector<std::uint8_t>(16, 0));
                },
                "movie_vertex_literals");
    readRefused("unreadable vertices", [](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    m.At(0x7018F7F0, false).bytes.resize(60);
                },
                "movie_geometry_extent");
    readRefused("no declaration", [&](FakeMemory& m, Tuple&, std::uint32_t&)
                {
                    at(m, 0x2E2C, 0);
                },
                "movie_missing_declaration");
    std::cout << "Reader refusals are named; stride, caller and pixel shader changes are foreign\n";

    // Adapter refusals on frame 900's real inputs, one rule each.
    auto            firstMemory = memoryAt(capture, first);
    MovieDrawInputs base;
    UNSCOPED_INFO(error);
    REQUIRE(ReadMovieDrawInputs(movieSubmission(first.arguments), readerFor(firstMemory), base, error));
    const auto refused = [&](const char* name, auto mutate, const std::string& expected, const NativeGameplayShaderInterface* other = nullptr)
    {
        auto broken = base;
        mutate(broken);
        MovieDrawRecipe rejected;
        std::string     reason;
        const bool      built = BuildMovieDrawRecipe(broken, other ? *other : shader, rejected, reason);
        UNSCOPED_INFO(std::string(name) + ": " + (built ? "accepted" : "[" + reason + "]"));
        REQUIRE((!built && reason.starts_with(expected) && !expected.empty()));
    };
    const auto plane = [](std::size_t slot, std::size_t index, std::uint32_t value)
    {
        return [=](MovieDrawInputs& in)
        {
            in.planes[slot].fetch[index] = value;
            store(in.state, kMovieFetchShadows[slot] + index * 4, value);
        };
    };
    refused("depth enabled", [](MovieDrawInputs& in)
            {
                store(in.state, 0x2934, 0x24F00772);
            },
            "movie_state 0x2200: 0x24f00772");
    refused("blend enabled", [](MovieDrawInputs& in)
            {
                store(in.state, 0x2938, 0x07060706);
            },
            "movie_state 0x2201: 0x7060706");
    refused("the viewport transform with the screen-space VS", [](MovieDrawInputs& in)
            {
                store(in.state, 0x294C, 0x43F);
            },
            "movie_state 0x2206: 0x43f");
    refused("guest clipping on", [](MovieDrawInputs& in)
            {
                store(in.state, 0x2944, 0x80000);
            },
            "movie_state 0x2204: 0x80000");
    refused("a color mask", [](MovieDrawInputs& in)
            {
                store(in.state, 0x28DC, 7);
            },
            "movie_state 0x2104: 0x7");
    refused("alpha test on", [](MovieDrawInputs& in)
            {
                store(in.state, 0x293C, 0x8700000F);
            },
            "movie_state 0x2202: 0x8700000f");
    refused("alpha test on with Greater", [](MovieDrawInputs& in)
            {
                store(in.state, 0x293C, 0x8700000C);
            },
            "movie_state 0x2202: 0x8700000c");
    refused("another RB_COLORCONTROL high byte", [](MovieDrawInputs& in)
            {
                store(in.state, 0x293C, 0x86000004);
            },
            "movie_state 0x2202: 0x86000004");
    refused("an alpha-to-mask offset", [](MovieDrawInputs& in)
            {
                store(in.state, 0x293C, 0x87000104);
            },
            "movie_state 0x2202: 0x87000104");
    // With the alpha test off, ALPHA_FUNC (bits 0..2) has no effect.
    for (const std::uint32_t function : { 0U, 4U })
    {
        auto relaxed = base;
        store(relaxed.state, 0x293C, 0x87000000 | function);
        MovieDrawRecipe built;
        UNSCOPED_INFO(std::format("alpha function {} with the alpha test off builds: {}", function, error));
        REQUIRE(BuildMovieDrawRecipe(relaxed, shader, built, error));
    }
    // The game-start movie draw's harvested state, with frame 900's planes
    // and vertices at its fetch words.
    if (const auto start = environment("REREVVED_MOVIE_GAME_START"); start.empty())
        std::cout << "Set REREVVED_MOVIE_GAME_START to the game-start movie harvest record to check its state\n";
    else
    {
        const std::filesystem::path record = start;
        auto                        game   = base;
        game.state                         = read(record / "state.be.bin");
        UNSCOPED_INFO("the game-start record holds RB_COLORCONTROL 0x87000004");
        REQUIRE((game.state.size() == 0x3500 && word(game.state, 0x293C) == 0x87000004));
        UNSCOPED_INFO("the game-start movie pair is frame 900's");
        REQUIRE((read(record / "vs_code.be.bin") == game.vertexCode && read(record / "ps_code.be.bin") == game.pixelCode));
        for (std::size_t slot = 0; slot < game.planes.size(); ++slot)
            for (std::size_t i = 0; i < 6; ++i)
                game.planes[slot].fetch[i] = word(game.state, kMovieFetchShadows[slot] + i * 4);
        MovieDrawRecipe built;
        UNSCOPED_INFO("the game-start movie state builds: " + error);
        REQUIRE(BuildMovieDrawRecipe(game, shader, built, error));
        std::cout << "The game-start movie state (RB_COLORCONTROL 0x87000004) builds\n";
    }
    auto flagless                = shader;
    flagless.screenSpacePosition = false;
    refused("the screen-space state without the flag", [](MovieDrawInputs&)
            {
            },
            "movie_shader_interface",
            &flagless);
    auto twoSlots              = shader;
    twoSlots.textureFetchSlots = { 0, 1 };
    refused("two texture slots", [](MovieDrawInputs&)
            {
            },
            "movie_shader_interface",
            &twoSlots);
    auto otherTarget              = shader;
    otherTarget.colorTargetFormat = 2;
    refused("another color target format", [](MovieDrawInputs&)
            {
            },
            "movie_recipe_rejected: ",
            &otherTarget);
    refused("a changed VS instruction", [](MovieDrawInputs& in)
            {
                in.vertexCode[0] ^= 1;
            },
            "movie_patched_vs_identity");
    refused("incomplete inputs", [](MovieDrawInputs& in)
            {
                in.inputsComplete = false;
            },
            "movie_inputs_incomplete");
    refused("primitive 5", [](MovieDrawInputs& in)
            {
                in.arguments.primitive = 5;
            },
            "movie_unsupported_nonindexed_up_tuple");
    refused("six vertices", [](MovieDrawInputs& in)
            {
                in.arguments.vertexCount = 6;
            },
            "movie_unsupported_nonindexed_up_tuple");
    refused("a predicated shader", [](MovieDrawInputs& in)
            {
                in.state[0x2ABE] |= 0x40;
            },
            "movie_conditional_or_recorded_shader");
    refused("a three-element declaration", [](MovieDrawInputs& in)
            {
                in.declaration.resize(36);
            },
            "movie_fetch_count");
    refused("a literal at c251", [](MovieDrawInputs& in)
            {
                in.literals[0].first = 0x47EC;
            },
            "movie_literal_shape");
    refused("a second literal range", [](MovieDrawInputs& in)
            {
                in.literalRanges.push_back(in.literalRanges[0]);
            },
            "movie_literal_shape");
    refused("label-form literal bytes", [](MovieDrawInputs& in)
            {
                in.pixelLiterals.assign(28, 0);
            },
            "movie_literal_shape");
    refused("a viewport shadow that disagrees", [](MovieDrawInputs& in)
            {
                store(in.state, 0x3168 + 8, 0x44000000);
            },
            "movie_viewport_shadow_scale_mismatch");
    refused("a plane fetch that differs from the state", [](MovieDrawInputs& in)
            {
                in.planes[1].fetch[3] ^= 1U << 19;
            },
            "movie_texture_fetch_mismatch");
    // One rule each for the remaining tuple, shader-state and literal fields.
    using Edit = std::pair<const char*, std::function<void(MovieDrawInputs&)>>;
    for (const auto& [name, edit] : std::initializer_list<Edit>{
             { "the call instruction as caller", [](MovieDrawInputs& in)
               {
                   in.arguments.caller = 0x82E39878;
               } },
             { "stride 16", [](MovieDrawInputs& in)
               {
                   in.arguments.stride = 16;
               } },
             { "a minimum vertex", [](MovieDrawInputs& in)
               {
                   in.arguments.minimumVertex = 1;
               } },
             { "an index count", [](MovieDrawInputs& in)
               {
                   in.arguments.indexCount = 4;
               } },
             { "unaligned vertices", [](MovieDrawInputs& in)
               {
                   in.arguments.vertices += 2;
               } },
             { "three retained vertices", [](MovieDrawInputs& in)
               {
                   in.vertices.resize(60);
               } },
         })
        refused(name, edit, "movie_unsupported_nonindexed_up_tuple");
    // The host clips depth while the guest does not; every captured Z is +0.0.
    for (const auto& [name, edit] : std::initializer_list<Edit>{
             { "every vertex at Z 2", [](MovieDrawInputs& in)
               {
                   for (std::uint32_t i = 0; i < kMovieVertices; ++i)
                       store(in.vertices, i * kMovieStride + 8, 0x40000000);
               } },
             { "the last vertex at Z 1", [](MovieDrawInputs& in)
               {
                   store(in.vertices, 3 * kMovieStride + 8, 0x3F800000);
               } },
             { "the first vertex at Z -0", [](MovieDrawInputs& in)
               {
                   store(in.vertices, 8, 0x80000000);
               } },
         })
        refused(name, edit, "movie_vertex_depth");
    refused("a predicated vertex shader", [](MovieDrawInputs& in)
            {
                in.state[0x2ABC] |= 0x40;
            },
            "movie_conditional_or_recorded_shader");
    refused("three vertex fetch records", [](MovieDrawInputs& in)
            {
                store(in.vertexMetadata, 28, 3);
            },
            "movie_fetch_count");
    for (const auto& [name, edit] : std::initializer_list<Edit>{
             { "the record at c248", [](MovieDrawInputs& in)
               {
                   in.literalRanges[0].firstFloat4 = 504;
               } },
             { "an eight-dword record", [](MovieDrawInputs& in)
               {
                   in.literalRanges[0].dwordCount = 8;
               } },
             { "fifteen literal registers", [](MovieDrawInputs& in)
               {
                   in.literals.pop_back();
               } },
             { "label-form vertex literal bytes", [](MovieDrawInputs& in)
               {
                   in.vertexLiterals.assign(28, 0);
               } },
         })
        refused(name, edit, "movie_literal_shape");
    // The decoded plane must be the one-level R8 form at the pinned size;
    // each field alone is refused.
    refused("a 1264-wide luma texture", [&](MovieDrawInputs& in)
            {
                plane(0, 2, 0x0059E4FF - 16)(in);
                in.planes[0].bytes.resize(1280 * 719 + 1264);
            },
            "movie_texture_form");
    refused("a 704-row luma texture", [&](MovieDrawInputs& in)
            {
                plane(0, 2, 0x0059E4FF - (16U << 13))(in);
                in.planes[0].bytes.resize(1280 * 704);
            },
            "movie_texture_form");
    refused("an identity swizzle", plane(2, 3, (0x01280248 & ~(0xFFFU << 1)) | (0x688U << 1)), "movie_texture_form");
    refused("a mipmapped plane", plane(0, 4, 3 | (1U << 6)), "movie_texture_mipmapped");
    refused("mip filter 3", plane(2, 3, 0x01280248 | (3U << 23)), "movie_sampler_unsupported");
    refused("a halfway clamp", plane(1, 0, (0x06024802 & ~(7U << 10)) | (4U << 10)), "movie_sampler_clamp_mode");
    refused("a texture border", plane(0, 3, 0x01280248 | 0x80000000U), "movie_sampler_border_size");
    refused("a truncated plane", [](MovieDrawInputs& in)
            {
                in.planes[0].bytes.resize(1000);
            },
            "movie_texture_decode: ");
    refused("a k_8_8_8_8 plane", [&](MovieDrawInputs& in)
            {
                plane(1, 1, 0x1AD79000 | 6)(in);
                in.planes[1].bytes.resize(768 * 359 * 4 + 640 * 4);
            },
            "movie_texture_form");
    std::cout << "Adapter refusals on frame 900 are named\n";

    // The control reaches the same state from the guest's own Original
    // outputs; the screen-space rule in the gameplay recipe is exact.
    Control control;
    controlInputs(capture, catalog, first, control);
    NativeDrawReplayRecipe controlDraw;
    UNSCOPED_INFO("control recipe: " + error);
    REQUIRE(controlRecipe(control, shader, controlDraw, error));
    UNSCOPED_INFO("the movie's screen-space state needs the specialized VS: " + error);
    REQUIRE((!controlRecipe(control, flagless, controlDraw, error) && error == "screen_space_position_unpinned"));
    control.registers[0x2206] = 0x43F;
    UNSCOPED_INFO("the specialized VS refuses the viewport transform: " + error);
    REQUIRE((!controlRecipe(control, shader, controlDraw, error) && error == "viewport_transform_unexpected"));
    control.registers[0x2206] = 0x400;
    std::cout << "The control recipe builds; screen_space_position_unpinned and viewport_transform_unexpected are exact\n";

    if (environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        std::cout << "REREVVED_TEST_NATIVE_D3D12 is not 1; the movie frames were not rendered\n";
        return 0;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the movie frames were not rendered\n";
        return 77;
    }

    // Movie acceptance checks tile-0 clear, quad, two scanouts, bit-exact
    // recipe and max RGB10 error 6.
    GuestTiling tiling;
    tiling.flags         = 0x20;
    tiling.count         = 2;
    tiling.rects[0]      = { 0, 0, 640, 720 };
    tiling.rects[1]      = { 640, 0, 1280, 720 };
    tiling.screenWidth   = 1280;
    tiling.screenHeight  = 720;
    tiling.predicateMask = 3;
    tiling.predicated    = 1;
    GuestClear clear;
    clear.target.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    clear.target.tiling   = tiling;
    clear.tiling          = tiling;
    clear.predicate       = 1;
    clear.flags           = 0x3F;
    clear.clear.rectangle = { 0, 0, 1280, 720 };
    clear.clear.color = clear.clear.depth = clear.clear.stencil = true;
    clear.clear.colorValue                                      = { 0, 0, 0, 1 };
    clear.depth                                                 = GuestDepthSurface{ 0x00700000, 0x530, 0x530, 1280, 720, 4, false };
    const auto movie                                            = std::make_shared<NativeGameplayShaderInterface>(shader);
    // Copy words come from hook state and Xenos reference; clear words seed
    // tile 1.
    const auto reference = environment("REREVVED_MOVIE_REFERENCE");
    UNSCOPED_INFO("the assembled movie frames take their copy control and info from REREVVED_MOVIE_REFERENCE");
    REQUIRE(!reference.empty());
    const auto controls = referenceCopyWords(reference);
    UNSCOPED_INFO("the Xenos reference frame copies two tiles");
    REQUIRE(controls.size() == 2);
    const auto copiesOf = [&](const Event& root, const Event& next)
    {
        const auto frame = "frame " + std::to_string(root.frame);
        UNSCOPED_INFO(frame + " has both tiles' captured base and pitch words");
        REQUIRE((root.resolves.size() == 2 && next.frame == root.frame + 1 && !next.resolves.empty() && root.resolves[1].previousCopy &&
                 next.resolves[0].previousCopy));
        std::vector<GuestResolveCopy> copies;
        for (std::uint32_t tile = 0; tile < 2; ++tile)
        {
            const auto& resolve = root.resolves[tile];
            // Hook copy words G+0x2A18..0x2A24 use the next entry's base/pitch
            // and Xenos control/info; clear words seed tile 1.
            const auto&      words = *(tile ? next.resolves[0] : root.resolves[1]).previousCopy;
            GuestResolveCopy copy;
            copy.copyControl = controls[tile].control;
            copy.destBase    = words[0];
            copy.destPitch   = words[1];
            copy.destInfo    = controls[tile].info;
            copy.depthClear  = controls[tile].depthClear;
            copy.colorClear  = controls[tile].colorClear;
            copy.rectX       = resolve.rectangle[0];
            copy.rectY       = resolve.rectangle[1];
            copy.rectWidth   = resolve.rectangle[2] - resolve.rectangle[0];
            copy.rectHeight  = resolve.rectangle[3] - resolve.rectangle[1];
            copy.destX       = resolve.point[0];
            copy.destY       = resolve.point[1];
            UNSCOPED_INFO(std::format("{} tile {} copy words {:08X},{:08X},{:08X},{:08X} are the title's scanout to its frontbuffer", frame, tile, copy.copyControl, copy.destBase, copy.destPitch, copy.destInfo));
            REQUIRE((copy.copyControl == (tile ? 0x00100040U : 0x00100340U) && (copy.copyControl & 0x300) == resolve.flags && copy.destInfo == 0x01000300 &&
                     copy.destPitch == 0x02D00500 && copy.destBase == root.frontbuffer && copy.destBase == resolve.base));
            copies.push_back(copy);
        }
        return copies;
    };
    const auto assembled = [&](const Event& root, const Event& next, bool swapChroma)
    {
        auto                       memory = memoryAt(capture, root);
        auto                       boot   = MakeBootDrawBuilder(readerFor(memory), nullptr, nullptr, nullptr, nullptr, movie);
        NativeFrameAssemblerConfig config;
        config.readback = true;
        config.build    = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (!boot(submission, draw, detail))
                return false;
            if (swapChroma)
                std::swap(draw.textures[1], draw.textures[2]);
            return true;
        };
        NativeFrameAssembler assembler(config);
        std::string          detail;
        UNSCOPED_INFO("clear admitted: " + detail);
        REQUIRE(assembler.Clear(clear, detail) != 0);
        auto submission      = movieSubmission(root.arguments);
        submission.stateRead = true;
        submission.sequence  = 1;
        assembler.Began(submission);
        const auto verdict = assembler.Admit(submission);
        UNSCOPED_INFO("movie draw admitted");
        REQUIRE(!verdict);
        assembler.Recorded(submission, verdict);
        const auto copies = copiesOf(root, next);
        for (std::uint32_t tile = 0; tile < 2; ++tile)
        {
            // 0x8250AC90 predicates each tile's scanout to that tile.
            auto target                  = clear.target;
            target.tiling->predicateMask = 3U << (2 * tile);
            bool partial                 = false;
            UNSCOPED_INFO("tile scanout: " + detail);
            REQUIRE((assembler.Resolve(target, copies[tile], true, partial, detail) != 0 && detail.empty()));
        }
        NativeFrameSwap swap;
        swap.sequence            = 1;
        swap.numberedThrough     = 1;
        swap.frontbufferPhysical = root.frontbuffer;
        swap.fetch               = root.presentFetch;
        swap.textureFormat       = 6;
        swap.width               = 1280;
        swap.height              = 720;
        swap.gammaTable          = gammaRamp();
        auto frame               = assembler.Swap(swap);
        UNSCOPED_INFO("one presented pass with the movie draw, read XYZ1: " + frame.detail);
        REQUIRE((frame.status == NativeFrameStatus::Provisional && frame.frame && frame.frame->passes.size() == 1 &&
                 frame.frame->passes[0].DrawCount() == 1 && frame.frame->outputSwizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 5 }));
        return std::move(*frame.frame);
    };
    const auto controlFrame = [&](NativeDrawReplayRecipe draw)
    {
        NativeRenderFrame frame;
        frame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
        NativeRenderFramePass pass;
        pass.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
        NativeRenderFramePass::Clear full;
        full.rectangle = { 0, 0, 1280, 720 };
        full.color = full.depth = full.stencil = true;
        full.colorValue                        = { 0, 0, 0, 1 };
        pass.clears.push_back(full);
        pass.draws.push_back(std::move(draw));
        NativeRenderFrameResolve resolve;
        resolve.textureId = 1;
        resolve.width     = 1280;
        resolve.height    = 720;
        resolve.samples   = { 0, 3 };
        pass.resolves.push_back(resolve);
        frame.passes.push_back(std::move(pass));
        frame.outputTextureId = 1;
        // The present's ZYX1 fetch under the scanout's red/blue swap.
        frame.outputSwizzle        = { 0, 1, 2, 5 };
        frame.gammaTable           = gammaRamp();
        frame.returnResolvedOutput = true;
        return frame;
    };

    const auto&         second = roots[1];
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    std::array<NativeDrawReplayResult, 2> actual, expected;
    NativeDrawReplayResult                swapped;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto& root = i ? second : first;
        Control     inputs;
        controlInputs(capture, catalog, root, inputs);
        NativeDrawReplayRecipe draw;
        UNSCOPED_INFO("control recipe: " + error);
        REQUIRE(controlRecipe(inputs, shader, draw, error));
        expected[i] = renderer.SubmitRenderFrame(controlFrame(std::move(draw)));
        actual[i]   = renderer.SubmitRenderFrame(assembled(root, roots[i + 1], false));
    }
    swapped = renderer.SubmitRenderFrame(assembled(first, second, true));
    renderer.Shutdown();
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto& root = i ? second : first;
        UNSCOPED_INFO("renders: " + expected[i].error + actual[i].error);
        REQUIRE((expected[i].success && actual[i].success));
        UNSCOPED_INFO("the assembled frame " + std::to_string(root.frame) + " equals the control bit-exactly");
        REQUIRE(actual[i].resolvedOutput == expected[i].resolvedOutput);
        const auto reference = referenceFrame(capture, root);
        const auto result    = compare(actual[i].resolvedOutput, reference, 6);
        std::cout << "Frame " << root.frame << " against the CPU YUV reference: max RGB10 error " << result.maximum << ", "
                  << result.over << " pixels above 6";
        if (result.over)
            std::cout << " in x " << result.left << ".." << result.right << ", y " << result.top << ".." << result.bottom << "; worst ("
                      << result.worst % 1280 << ", " << result.worst / 1280 << ") " << result.actual[0] << "/" << result.actual[1] << "/"
                      << result.actual[2] << " vs " << result.expected[0] << "/" << result.expected[1] << "/" << result.expected[2];
        std::cout << "\n";
        UNSCOPED_INFO("frame " + std::to_string(root.frame) + " exceeds the RGB10 error bound");
        REQUIRE(result.maximum <= 6);
        if (i == 0)
        {
            UNSCOPED_INFO("swapped render: " + swapped.error);
            REQUIRE(swapped.success);
            const auto bad = compare(swapped.resolvedOutput, reference, 6);
            std::cout << "Swapped chroma planes: max RGB10 error " << bad.maximum << ", " << bad.over << " pixels above 6\n";
            UNSCOPED_INFO("the error bound detects swapped chroma planes");
            REQUIRE(bad.maximum > 6);
        }
    }
    std::cout << "Assembled movie frames equal the control and the CPU YUV reference within 6 RGB10 units\n";
    return 0;
}

// The largest RGB10 channel error over the pixels a predicate selects.
unsigned maximumError(const std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& reference, const std::function<bool(unsigned, unsigned)>& selected)
{
    unsigned maximum = 0;
    for (unsigned y = 0; y < 720; ++y)
        for (unsigned x = 0; x < 1280; ++x)
            if (selected(x, y))
            {
                const auto a = little(output, (y * 1280 + x) * 4), b = little(reference, (y * 1280 + x) * 4);
                for (unsigned c = 0; c < 3; ++c)
                    maximum = std::max(maximum, unsigned(std::abs(int((a >> (c * 10)) & 1023) - int((b >> (c * 10)) & 1023))));
            }
    return maximum;
}

// Native control replay is compared with the owned Xenos RGB10 frame.
int xenosReference(const std::filesystem::path& directory, const std::filesystem::path& catalog)
{
    std::string                   error;
    NativeGameplayShaderInterface shader;
    UNSCOPED_INFO("movie catalog: " + error);
    REQUIRE(LoadMovieDrawShader(catalog, shader, error));
    const auto input    = directory / "input";
    const auto report   = toml::parse_file((directory / "reference.toml").string());
    const auto manifest = toml::parse_file((input / "manifest.toml").string());
    const auto file     = [&](const toml::node_view<const toml::node>& entry)
    {
        return read(input / entry["file"].value_or(std::string()));
    };
    UNSCOPED_INFO("the frame reference is complete");
    REQUIRE(report["status"].value_or(std::string()) == "complete");
    UNSCOPED_INFO("a 1280x720 RGB10 output through the 256-entry gamma table");
    REQUIRE((manifest["output_format"].value_or<std::int64_t>(0) == 24 && manifest["output_width"].value_or<std::int64_t>(0) == 1280 &&
             manifest["output_height"].value_or<std::int64_t>(0) == 720 && !manifest["use_fxaa"].value_or(true) &&
             !manifest["use_pwl_gamma"].value_or(true) && manifest["is_8bpc"].value_or(false)));

    const auto wordsOf = [&](const toml::node_view<const toml::node>& entry)
    {
        const auto                 bytes = file(entry);
        std::vector<std::uint32_t> result(bytes.size() / 4);
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = little(bytes, i * 4);
        return result;
    };
    const auto* events = manifest["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE((events && !events->empty()));
    std::vector<const toml::table*>         entries;
    std::vector<std::vector<std::uint32_t>> registers;
    for (const auto& node : *events)
    {
        UNSCOPED_INFO("event rows");
        REQUIRE(node.is_table());
        entries.push_back(node.as_table());
        registers.push_back(wordsOf((*entries.back())["registers"]));
        UNSCOPED_INFO("a full register file");
        REQUIRE(registers.back().size() == 0x5003);
    }
    const auto kind = [&](std::size_t i)
    {
        return (*entries[i])["kind"].value_or<std::int64_t>(-1);
    };
    const auto text = [&](std::size_t i, const char* key)
    {
        return (*entries[i])[key].value_or(std::string());
    };
    const auto name = [&](std::size_t i)
    {
        return "event " + std::to_string((*entries[i])["id"].value_or<std::int64_t>(0));
    };

    // Harmless points are inert; other writers must precede the opaque quad.
    UNSCOPED_INFO("the frame runs from one swap to the next");
    REQUIRE((entries.size() >= 2 && kind(0) == 2 && kind(entries.size() - 1) == 2));
    const auto& swap        = *entries.back();
    const auto  frontbuffer = std::uint32_t(swap["frontbuffer"].value_or<std::int64_t>(0));
    UNSCOPED_INFO("the presented frontbuffer is 1280x720");
    REQUIRE((swap["frontbuffer_width"].value_or<std::int64_t>(0) == 1280 && swap["frontbuffer_height"].value_or<std::int64_t>(0) == 720));
    const auto issued = [&](std::size_t i)
    {
        return (*entries[i])["success"].value_or(false) && (*entries[i])["host_issued"].value_or(false);
    };
    std::vector<std::size_t> tiles, copies;
    std::size_t              earlier = 0, inert = 0, lastWriter = 0;
    for (std::size_t i = 1; i + 1 < entries.size(); ++i)
    {
        const auto& r = registers[i];
        if (kind(i) == 1)
        {
            UNSCOPED_INFO(name(i) + " follows its resolve packet");
            REQUIRE((kind(i - 1) == 0 && (registers[i - 1][kModeControl] & 7) == 6 && registers[i - 1] == r));
            UNSCOPED_INFO(name(i) + " completed its resolve");
            REQUIRE(((*entries[i])["success"].value_or(false) && text(i, "copy_mode") == "resolve"));
            copies.push_back(i);
            continue;
        }
        UNSCOPED_INFO(name(i) + " is a draw or a copy");
        REQUIRE(kind(i) == 0);
        if ((r[kModeControl] & 7) == 6)
        {
            UNSCOPED_INFO(name(i) + " is a resolve packet");
            REQUIRE((!(*entries[i])["host_issued"].value_or(true) && (*entries[i])["success"].value_or(false) && kind(i + 1) == 1));
            continue;
        }
        if (text(i, "guest_vertex_hash") == "C33871A8CFA8967A" && text(i, "guest_pixel_hash") == "A8D1E41B3FBB2D15")
        {
            UNSCOPED_INFO(name(i) + " is the movie pair's four-vertex strip");
            REQUIRE(((*entries[i])["primitive"].value_or<std::int64_t>(0) == 6 && (*entries[i])["requested_count"].value_or<std::int64_t>(0) == 4 &&
                     !(*entries[i])["indexed"].value_or(true)));
            UNSCOPED_INFO(name(i) + " was issued to the host");
            REQUIRE(issued(i));
            tiles.push_back(i);
            continue;
        }
        if (r[kColorMask] == 0 && (r[kDepthControl] & 7) == 0 && !exportsMemory(wordsOf((*entries[i])["vertex_ucode"])) &&
            !exportsMemory(wordsOf((*entries[i])["pixel_ucode"])))
        {
            ++inert;
            continue;
        }
        UNSCOPED_INFO(name(i) + " writes after the first movie draw");
        REQUIRE(tiles.empty());
        ++earlier;
        lastWriter = i;
    }

    // Tile copies swap red/blue and start at 32-aligned columns.
    UNSCOPED_INFO("one copy per movie tile");
    REQUIRE((!tiles.empty() && tiles.size() == copies.size()));
    const auto                                           pitch = registers[tiles.front()][kSurfaceInfo] & 0x3FFF;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> columns;
    const auto&                                          first = *entries[tiles.front()];
    const auto                                           sha   = [](const toml::table& e, const char* key)
    {
        return e[key]["sha256"].value_or(std::string());
    };
    const auto fetchOf = [](const toml::table& e)
    {
        const auto* fetches = e["vertex_fetches"].as_array();
        UNSCOPED_INFO("one vertex fetch");
        REQUIRE((fetches && fetches->size() == 1 && (*fetches)[0].is_table()));
        return (*fetches)[0].as_table();
    };
    std::uint32_t               covered = 0;
    std::vector<PixelRectangle> footprints;
    for (std::size_t t = 0; t < tiles.size(); ++t)
    {
        const auto& e    = *entries[tiles[t]];
        const auto& r    = registers[tiles[t]];
        auto        same = registers[tiles.front()];
        for (const auto index : { kRingWritePointer, kWindowOffset, kWindowScissorTl, kWindowScissorBr })
            same[index] = r[index];
        const auto *uses = e["texture_uses"].as_array(), *firstUses = first["texture_uses"].as_array();
        UNSCOPED_INFO(name(tiles[t]) + " replays the first movie tile");
        REQUIRE((same == r && sha(e, "vertex_ucode") == sha(first, "vertex_ucode") && sha(e, "pixel_ucode") == sha(first, "pixel_ucode") &&
                 (*fetchOf(e))["data"]["sha256"].value_or(std::string()) == (*fetchOf(first))["data"]["sha256"].value_or(std::string()) &&
                 uses && firstUses && *uses == *firstUses));
        const auto left = r[kWindowScissorTl] & 0x3FFF, top = (r[kWindowScissorTl] >> 16) & 0x3FFF, right = r[kWindowScissorBr] & 0x3FFF,
                   bottom   = (r[kWindowScissorBr] >> 16) & 0x3FFF;
        const auto signed15 = [](std::uint32_t value)
        {
            return std::int32_t(value << 17) >> 17;
        };
        UNSCOPED_INFO(name(tiles[t]) + " is the next column tile at its EDRAM origin");
        REQUIRE((!(r[kWindowScissorTl] >> 31) && left == covered && right > left && right - left <= pitch && top == 0 && bottom == 720 &&
                 signed15(r[kWindowOffset] & 0x7FFF) == -std::int32_t(left) && signed15((r[kWindowOffset] >> 16) & 0x7FFF) == 0));
        covered = right;
        columns.emplace_back(left, right);

        const auto copy = copies[t];
        UNSCOPED_INFO(name(copy) + " resolves its tile");
        REQUIRE((copy > tiles[t] && (t + 1 == tiles.size() || copy < tiles[t + 1])));
        const auto& c = registers[copy];
        UNSCOPED_INFO(name(copy) + " converts color 0 samples 0 and 1 under its tile's window into the frontbuffer layout");
        REQUIRE((c[kWindowOffset] == r[kWindowOffset] && c[kWindowScissorTl] == r[kWindowScissorTl] && c[kWindowScissorBr] == r[kWindowScissorBr] &&
                 ((c[kSuModeControl] >> 16) & 1) && (c[kCopyControl] & ~0x300U) == 0x00100040 && c[kCopyDestPitch] == ((720U << 16) | 1280U) &&
                 c[kCopyDestInfo] == 0x01000300));
        UNSCOPED_INFO(name(copy) + " lands on its columns of the presented frontbuffer");
        REQUIRE((left % 32 == 0 && c[kCopyDestBase] == frontbuffer + std::uint32_t(rex::graphics::texture_util::GetTiledOffset2D(std::int32_t(left), 0, 1280, 2))));
        if (t)
        {
            const auto& previous = registers[copies[t - 1]];
            UNSCOPED_INFO(name(copies[t - 1]) + " clears the EDRAM of " + name(tiles[t]) + " to zero");
            REQUIRE((((previous[kCopyControl] >> 8) & 1) && previous[kColorClear] == 0 && previous[kColorClearLow] == 0 &&
                     right - left <= columns[t - 1].second - columns[t - 1].first));
        }
        // Resolve footprint must stay within the tile draw's EDRAM columns.
        UNSCOPED_INFO(name(copy) + " resolves the color surface of " + name(tiles[t]));
        REQUIRE(((c[kColorInfo] & 0x000F0FFF) == (r[kColorInfo] & 0x000F0FFF) && (c[kSurfaceInfo] & 0x00033FFF) == (r[kSurfaceInfo] & 0x00033FFF)));
        const auto& rectangle = *fetchOf(*entries[copy]);
        UNSCOPED_INFO(name(copy) + " captures vertex fetch 0");
        REQUIRE(rectangle["slot"].value_or<std::int64_t>(-1) == 0);
        const auto footprint = resolveFootprint(c, std::uint64_t(rectangle["base"].value_or<std::int64_t>(-1)), file(rectangle["data"]), name(copy));
        const auto drawn     = scissorOf(*registerFile(r), true);
        UNSCOPED_INFO(name(copy) + " resolves only pixels its tile can draw");
        REQUIRE((footprint.left >= drawn.left && footprint.top >= drawn.top && footprint.right <= drawn.right && footprint.bottom <= drawn.bottom));
        UNSCOPED_INFO(name(tiles[t]) + " can draw exactly its columns");
        REQUIRE(drawn == PixelRectangle{ 0, 0, std::int32_t(right - left), 720 });
        // Base plus EDRAM column x is frontbuffer column left + x.
        footprints.push_back({ std::int32_t(left) + footprint.left, footprint.top, std::int32_t(left) + footprint.right, footprint.bottom });
    }
    UNSCOPED_INFO("the movie tiles cover all 1280 columns");
    REQUIRE(covered == 1280);
    // The effective resolve footprints, in frontbuffer pixels, partition the
    // compared 1280x720 frame.
    std::int32_t resolved = 0;
    for (const auto& footprint : footprints)
    {
        UNSCOPED_INFO("the resolves continue the frame at column " + std::to_string(resolved));
        REQUIRE((footprint.left == resolved && footprint.right > footprint.left && footprint.top == 0 && footprint.bottom == 720));
        resolved = footprint.right;
    }
    UNSCOPED_INFO("the resolves cover the frame's 1280 columns");
    REQUIRE(resolved == 1280);

    // The opaque quad covers the frame: it replaces every RT0 sample it
    // covers, without depth or stencil, and has corners at (0, 0) and
    // (1280, 720).
    const auto& movie = registers[tiles.front()];
    UNSCOPED_INFO("the movie draw replaces RT0 without depth or stencil");
    REQUIRE((replacesColor(movie) && movie[kBlendControl0] == 0x00010001 && (movie[kDepthControl] & 7) == 0));

    // Half-pixel edges include prior tile clear content; later tiles begin from
    // zero.
    UNSCOPED_INFO("a clear precedes the first movie tile");
    REQUIRE(lastWriter != 0);
    const auto&                                    clearEntry = *entries[lastWriter];
    rex::graphics::diagnostic::DrawCaptureSnapshot clearDraw;
    clearDraw.registers             = registers[lastWriter];
    clearDraw.vertex_ucode          = wordsOf(clearEntry["vertex_ucode"]);
    clearDraw.pixel_ucode           = wordsOf(clearEntry["pixel_ucode"]);
    clearDraw.primitive_type        = std::uint32_t(clearEntry["primitive"].value_or<std::int64_t>(0));
    clearDraw.requested_index_count = std::uint32_t(clearEntry["requested_count"].value_or<std::int64_t>(0));
    clearDraw.index.present         = clearEntry["indexed"].value_or(true);
    if (const auto* fetches = clearEntry["vertex_fetches"].as_array())
        for (const auto& node : *fetches)
        {
            const auto&                                       fetch = *node.as_table();
            rex::graphics::diagnostic::DrawCaptureVertexFetch captured;
            captured.fetch_constant = std::uint32_t(fetch["slot"].value_or<std::int64_t>(-1));
            captured.base           = std::uint32_t(fetch["base"].value_or<std::int64_t>(0));
            captured.bytes          = file(fetch["data"]);
            clearDraw.vertex_fetches.push_back(std::move(captured));
        }
    UNSCOPED_INFO(name(lastWriter) + " was issued to the host");
    REQUIRE(issued(lastWriter));
    UNSCOPED_INFO(name(lastWriter) + " replaces every color sample it covers");
    REQUIRE(replacesColor(clearDraw.registers));
    NativeRenderFramePass clearPass;
    const bool            decoded = BuildNativeGameplayClearPass(clearDraw, clearPass, error);
    UNSCOPED_INFO(name(lastWriter) + " is the title's rectangle clear: " + error);
    REQUIRE(decoded);
    UNSCOPED_INFO(name(lastWriter) + " clears one rectangle");
    REQUIRE(clearPass.clears.size() == 1);
    const auto& initial = clearPass.clears.front();
    UNSCOPED_INFO(name(lastWriter) + " clears the movie's whole color surface");
    REQUIRE((clearDraw.registers[kColorInfo] == movie[kColorInfo] && initial.color && clearPass.target.width == pitch &&
             clearPass.target.height == 720 && initial.rectangle.left == 0 && initial.rectangle.top == 0 &&
             initial.rectangle.right == std::int32_t(pitch) && initial.rectangle.bottom == 720));
    // The tiles partition the frame, so one draw under the full-frame window
    // (the guest's pre-tiling scissor) renders their union.
    const auto& event = first;
    Control     control;
    control.registers                   = movie;
    control.registers[kWindowOffset]    = 0;
    control.registers[kWindowScissorTl] = 0;
    control.registers[kWindowScissorBr] = (720U << 16) | 1280U;
    control.vs                          = wordsOf(event["vertex_ucode"]);
    control.ps                          = wordsOf(event["pixel_ucode"]);
    std::vector<std::uint8_t> pixelBytes(control.ps.size() * 4);
    for (std::size_t i = 0; i < control.ps.size(); ++i)
        store(pixelBytes, i * 4, control.ps[i]);
    UNSCOPED_INFO("the Xenos pixel shader is the pinned movie PS");
    REQUIRE(MovieMicrocodeWordDigest(pixelBytes) == kMoviePixelWordDigest);
    UNSCOPED_INFO("the Xenos vertex shader is the catalog's patched VS");
    REQUIRE(control.vs == words(read(catalog / "vs.ucode.be.bin")));

    const auto& vertexFetch = *fetchOf(event);
    UNSCOPED_INFO("the vertex fetch is constant 95");
    REQUIRE(vertexFetch["slot"].value_or<std::int64_t>(-1) == 95);
    control.vertices = file(vertexFetch["data"]);
    UNSCOPED_INFO("four 20-byte vertices");
    REQUIRE(control.vertices.size() >= kMovieVertices * kMovieStride);
    control.vertices.resize(kMovieVertices * kMovieStride);
    std::set<std::pair<float, float>> corners;
    for (std::uint32_t i = 0; i < kMovieVertices; ++i)
    {
        UNSCOPED_INFO("every Xenos vertex Z is +0.0");
        REQUIRE(word(control.vertices, i * kMovieStride + 8) == 0);
        corners.emplace(std::bit_cast<float>(word(control.vertices, i * kMovieStride)), std::bit_cast<float>(word(control.vertices, i * kMovieStride + 4)));
    }
    UNSCOPED_INFO("the movie quad spans the full frame");
    REQUIRE((corners == std::set<std::pair<float, float>>{ { 0.0F, 0.0F }, { 1280.0F, 0.0F }, { 0.0F, 720.0F }, { 1280.0F, 720.0F } }));
    std::cout << "Xenos frame " << report["start_frame"].value_or<std::int64_t>(0) << ": " << tiles.size()
              << " issued movie tile draws with their resolves onto the frontbuffer columns, " << earlier << " earlier draw(s) ending with the "
              << name(lastWriter) << " surface clear, " << inert << " point draws without color, depth, stencil or memory writes\n";
    control.registers[0x48BF] = static_cast<std::uint32_t>(control.vertices.size()) | 0x10000002U;

    // Each plane's host rows rebuilt at the guest pitch and decoded as the
    // S11 control decodes guest bytes.
    const auto* uses     = event["texture_uses"].as_array();
    const auto* textures = manifest["textures"].as_array();
    UNSCOPED_INFO("texture uses and textures");
    REQUIRE((uses && textures));
    for (std::size_t slot = 0; slot < 3; ++slot)
    {
        // The translator also binds a signed companion; an unsigned fetch never
        // samples it, so it must be the unavailable one.
        NativeTextureFetch fetch{};
        for (std::size_t i = 0; i < 6; ++i)
            fetch[i] = control.registers[0x4800 + slot * 6 + i];
        const toml::table* use = nullptr;
        for (const auto& node : *uses)
            if (const auto& candidate = *node.as_table(); candidate["fetch"].value_or<std::int64_t>(-1) == std::int64_t(slot))
            {
                if (candidate["signed"].value_or(true))
                {
                    UNSCOPED_INFO("plane " + std::to_string(slot) + " is unsigned");
                    REQUIRE((!candidate["available"].value_or(true) && ((fetch[0] >> 2) & 0xFF) == 0));
                    continue;
                }
                UNSCOPED_INFO("one unsigned texture per plane fetch");
                REQUIRE(use == nullptr);
                use = &candidate;
            }
        UNSCOPED_INFO("plane " + std::to_string(slot) + " is available");
        REQUIRE((use != nullptr && (*use)["available"].value_or(false)));
        const auto& texture = *(*textures)[std::size_t((*use)["texture"].value_or<std::int64_t>(0))].as_table();
        const auto  width = std::uint32_t(texture["width"].value_or<std::int64_t>(0)), height = std::uint32_t(texture["height"].value_or<std::int64_t>(0));
        const auto  pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
        const auto  rows  = file(texture["data"]);
        // DXGI R8_TYPELESS or R8_UNORM.
        const auto format = texture["format"].value_or<std::int64_t>(0);
        UNSCOPED_INFO("plane " + std::to_string(slot) + " is an R8 host texture at the movie size");
        REQUIRE(((format == 60 || format == 61) && width == (slot ? 640U : 1280U) && height == (slot ? 360U : 720U) &&
                 texture["row_size"].value_or<std::int64_t>(0) == width && rows.size() == std::size_t(width) * height && pitch >= width));
        std::vector<std::uint8_t> guest(std::size_t(pitch) * (height - 1) + width);
        for (std::uint32_t y = 0; y < height; ++y)
            std::copy_n(rows.begin() + std::size_t(y) * width, width, guest.begin() + std::size_t(y) * pitch);
        UNSCOPED_INFO("plane " + std::to_string(slot) + ": " + error);
        REQUIRE(DecodeNativeTexture(fetch, guest, control.textures[slot], error));
        auto& sampler     = control.samplers[slot];
        sampler.address   = { 3, 3, 3 };
        sampler.magLinear = sampler.minLinear = true;
        sampler.maxLod                        = std::numeric_limits<float>::max();
    }
    const auto                     gammaBytes = file(manifest["gamma"]);
    std::array<std::uint32_t, 256> gamma{};
    UNSCOPED_INFO("a 256-entry gamma table");
    REQUIRE(gammaBytes.size() == gamma.size() * 4);
    for (std::size_t i = 0; i < gamma.size(); ++i)
        gamma[i] = little(gammaBytes, i * 4);
    const auto reference = file(manifest["reference"]);
    UNSCOPED_INFO("a 1280x720 RGB10 reference");
    REQUIRE(reference.size() == 1280 * 720 * 4);

    if (environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        std::cout << "REREVVED_TEST_NATIVE_D3D12 is not 1; the Xenos movie frame was not compared\n";
        return 77;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the Xenos movie frame was not compared\n";
        return 77;
    }
    NativeDrawReplayRecipe recipe;
    UNSCOPED_INFO("control recipe: " + error);
    REQUIRE(controlRecipe(control, shader, recipe, error));
    NativeRenderFrame frame;
    frame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    NativeRenderFramePass pass;
    pass.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    for (std::size_t t = 0; t < columns.size(); ++t)
    {
        NativeRenderFramePass::Clear clear;
        clear.rectangle = { std::int32_t(columns[t].first), 0, std::int32_t(columns[t].second), 720 };
        clear.color = clear.depth = clear.stencil = true;
        clear.colorValue                          = t ? std::array<float, 4>{} : initial.colorValue;
        pass.clears.push_back(clear);
    }
    pass.draws.push_back(std::move(recipe));
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = 1280;
    resolve.height    = 720;
    resolve.samples   = { 0, 3 };
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    frame.outputTextureId      = 1;
    frame.gammaTable           = gamma;
    frame.returnResolvedOutput = true;
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto result = renderer.SubmitRenderFrame(std::move(frame));
    renderer.Shutdown();
    UNSCOPED_INFO("native render: " + result.error);
    REQUIRE((result.success && result.resolvedOutput.size() == reference.size()));

    const auto all    = maximumError(result.resolvedOutput, reference, [](unsigned, unsigned)
                                     {
                                      return true;
                                     });
    const auto row    = maximumError(result.resolvedOutput, reference, [](unsigned, unsigned y)
                                     {
                                      return y == 0;
                                     });
    const auto column = maximumError(result.resolvedOutput, reference, [](unsigned x, unsigned)
                                     {
                                         return x == 0;
                                     });
    const auto inner  = maximumError(result.resolvedOutput, reference, [](unsigned x, unsigned y)
                                     {
                                        return x && y;
                                     });
    std::cout << "Xenos frame " << report["start_frame"].value_or<std::int64_t>(0) << " against native: max RGB10 error " << all << " over the full frame, "
              << row << " on row 0, " << column << " on column 0, " << inner << " elsewhere\n";
    UNSCOPED_INFO("the native movie frame exceeds the RGB10 error bound against Xenos");
    REQUIRE(all <= 6);
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

TEST_CASE("movie_draw_adapter", "[ctest][movie-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
}

TEST_CASE("movie_draw_adapter_capture", "[.][renderer-research][movie-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset capture skips; a set but unusable capture or catalog fails.
    const auto capture = environment("REREVVED_MOVIE_CAPTURE");
    if (capture.empty())
    {
        SKIP("Set REREVVED_MOVIE_CAPTURE to the S11 movie capture directory to run this check");
    }
    const auto catalog = environment("REREVVED_MOVIE_SHADERS");
    UNSCOPED_INFO("REREVVED_MOVIE_CAPTURE is set; the movie checks also need REREVVED_MOVIE_SHADERS");
    REQUIRE(!catalog.empty());
    skipIfUnavailable(s11(capture, catalog), "no D3D12 device for the movie capture");
}

TEST_CASE("movie_draw_adapter_reference", "[.][renderer-research][movie-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    const auto                     reference = environment("REREVVED_MOVIE_REFERENCE");
    if (reference.empty())
    {
        SKIP("Set REREVVED_MOVIE_REFERENCE to a Xenos movie frame reference directory to run this check");
    }
    const auto catalog = environment("REREVVED_MOVIE_SHADERS");
    UNSCOPED_INFO("REREVVED_MOVIE_REFERENCE is set; the comparison also needs REREVVED_MOVIE_SHADERS");
    REQUIRE(!catalog.empty());
    skipIfUnavailable(xenosReference(reference, catalog), "no D3D12 device for the movie reference");
}
