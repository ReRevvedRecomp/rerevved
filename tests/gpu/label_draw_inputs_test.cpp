#include "gpu/guest_d3d/label_draw_inputs.h"

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/label_draw_capture.h"
#endif

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <toml++/toml.hpp>

using namespace rerevved::gpu::guest_d3d;
#if REREVVED_DEVELOPER
namespace diagnostics = rerevved::gpu::diagnostics;
#endif

namespace
{

constexpr std::uint32_t kBase = 0x10000, kGraphics = kBase + 0x10000;
constexpr std::uint32_t kVs = kBase + 0x1000, kPs = kBase + 0x2000, kDeclaration = kBase + 0x3000;
constexpr std::uint32_t kVertices = kBase + 0x4000, kPatched = kBase + 0x5000, kRing = kBase + 0x6000;
constexpr std::uint32_t kTexture = 0x100000, kTextureBytes = 1024 * 1024, kVertexCount = 6;

std::string lastError;

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// Guest memory for one label batch. The pixel shader is a fixture, so reads
// use its digest instead of the retail pixel shader's.
struct Guest
{
    std::vector<std::uint8_t> virtualMemory  = std::vector<std::uint8_t>(0x20000, 0);
    std::vector<std::uint8_t> physicalMemory = std::vector<std::uint8_t>(0x200000, 0xEE);
    unsigned                  reads          = 0;
    bool                      failTexture    = false;

    std::uint8_t* at(std::uint32_t address)
    {
        return virtualMemory.data() + (address - kBase);
    }

    Guest()
    {
        auto& m = virtualMemory;
        store(m, kGraphics - kBase + 0x3198, kVs);
        store(m, kGraphics - kBase + 0x3194, kPs);
        store(m, kGraphics - kBase + 0x2E2C, kDeclaration);
        // k_8 1024x1024, linear, pitch 32, one level; swizzle 0,0,0,R.
        const std::array<std::uint32_t, 6> fetch{ 0x08000002, kTexture | 2, 0x007FE3FF, 0x248 | (1U << 19) | (1U << 21), 0, 0x200 };
        for (std::size_t i = 0; i < fetch.size(); ++i)
            store(m, kGraphics - kBase + 0x480 + i * 4, fetch[i]);
        // VS: metadata at +1000, code resource 0x300 (physical), 12 words.
        store(m, kVs - kBase + 896, 128);
        store(m, kVs - kBase + 32, 0x300);
        store(m, kVs - kBase + 1000 + 4, 48);
        store(m, kVs - kBase + 1000 + 28, 3);
        for (unsigned i = 0; i < 3; ++i)
        {
            store(m, kVs - kBase + 1000 + 36 + i * 4, 0x1000 + i);
            for (unsigned j = 0; j < 12; ++j)
                m[kVs - kBase + 68 + i * 12 + j] = static_cast<std::uint8_t>(0x40 + i * 12 + j);
        }
        // PS: metadata at +168, code resource 0x1000 (physical), 9 words.
        store(m, kPs - kBase + 64, 128);
        store(m, kPs - kBase + 24, 0x1000);
        store(m, kPs - kBase + 168 + 4, 36);
        for (unsigned i = 0; i < 36; ++i)
            physicalMemory[0x1000 + i] = static_cast<std::uint8_t>(0x80 + i);
        for (unsigned i = 0; i < 48; ++i)
            physicalMemory[0x300 + i] = static_cast<std::uint8_t>(0x10 + i);
        // Declaration: three 12-byte elements after a 52-byte header.
        store(m, kDeclaration - kBase + 24, 3);
        for (unsigned i = 0; i < 36; ++i)
            m[kDeclaration - kBase + 52 + i] = static_cast<std::uint8_t>(0x60 + i);
        for (unsigned i = 0; i < kVertexCount * kLabelStride; ++i)
            m[kVertices - kBase + i] = static_cast<std::uint8_t>(i * 7 + 3);
        for (std::uint32_t i = 0; i < kTextureBytes; ++i)
            physicalMemory[kTexture + i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));
    }

    std::vector<std::uint8_t> read(std::uint64_t address, std::uint64_t size, bool physical)
    {
        ++reads;
        if (!physical && address == 0x82091394)
            return std::vector<std::uint8_t>(size, 0x11);
        if (!physical && address == 0x820913A8)
            return std::vector<std::uint8_t>(size, 0x22);
        const auto& memory = physical ? physicalMemory : virtualMemory;
        const auto  base   = physical ? 0U : kBase;
        if (address < base || address - base + size > memory.size() || (physical && failTexture && address == kTexture))
            throw std::runtime_error("unreadable guest memory " + std::to_string(address) + "+" + std::to_string(size) + (physical ? " phys" : " virt"));
        return { memory.begin() + (address - base), memory.begin() + (address - base + size) };
    }
};

NativeDrawSubmission submission()
{
    NativeDrawSubmission s;
    s.hook      = NativeDrawHook::NonindexedUp;
    s.caller    = kLabelDrawCaller;
    s.arguments = { kGraphics, 4, kVertexCount, kVertices, kLabelStride, 0, 0, 0 };
    return s;
}

} // namespace

TEST_CASE("label_draw_inputs", "[ctest][label-draw-inputs][timeout-60]")
{
    Guest            guest;
    const ReadMemory read = [&](auto address, auto size, auto physical)
    {
        return guest.read(address, size, physical);
    };
    const auto      psCode = std::vector<std::uint8_t>(guest.physicalMemory.begin() + 0x1000, guest.physicalMemory.begin() + 0x1024);
    const auto      digest = LabelMicrocodeWordDigest(psCode);
    std::string&    error  = lastError;
    LabelDrawInputs inputs;

    // Selection: only the 0x82304258 nonindexed-UP triangle-list tuple is read.
    const auto foreign = [&](NativeDrawSubmission s, const char* message)
    {
        guest.reads = 0;
        UNSCOPED_INFO(message);
        REQUIRE((!ReadLabelDrawInputsForDigest(digest, s, read, inputs, error) && error.empty() && guest.reads == 0 &&
                 !inputs.labelCandidate && inputs.state.empty()));
    };
    auto s = submission();
    s.hook = NativeDrawHook::IndexedUp;
    foreign(s, "indexed-UP hook is foreign");
    s        = submission();
    s.caller = 0x82E3987C;
    foreign(s, "the movie call site is foreign");
    s              = submission();
    s.arguments[1] = 5;
    foreign(s, "primitive 5 is foreign");
    s              = submission();
    s.arguments[4] = 8;
    foreign(s, "stride 8 is foreign");
    s              = submission();
    s.arguments[2] = 7;
    foreign(s, "a vertex count that is not a multiple of three is foreign");
    s.arguments[2] = 0;
    foreign(s, "an empty batch is foreign");

    // A pixel shader other than the pinned one is foreign, with no later reads.
    UNSCOPED_INFO("the retail pixel shader digest does not admit a fixture shader");
    REQUIRE((!ReadLabelDrawInputs(submission(), read, inputs, error) && error.empty() && !inputs.labelCandidate &&
             inputs.vertices.empty() && inputs.vertexCode.empty()));
    UNSCOPED_INFO("fixture microcode is not the retail pixel shader");
    REQUIRE(!IsLabelPixelShader(psCode));

    // Bytes: the admitted batch holds the CPU vertices and the whole texture.
    UNSCOPED_INFO("label batch is admitted");
    REQUIRE((ReadLabelDrawInputsForDigest(digest, submission(), read, inputs, error) && error.empty()));
    UNSCOPED_INFO("admission sets candidate and completion");
    REQUIRE((inputs.labelCandidate && inputs.inputsComplete));
    UNSCOPED_INFO("nonindexed tuple maps onto the shared argument record");
    REQUIRE((inputs.arguments.graphics == kGraphics && inputs.arguments.primitive == 4 && inputs.arguments.vertexCount == kVertexCount &&
             inputs.arguments.vertices == kVertices && inputs.arguments.stride == kLabelStride && inputs.arguments.indexCount == 0 &&
             inputs.arguments.caller == kLabelDrawCaller));
    UNSCOPED_INFO("vertex bytes are the CPU staging bytes");
    REQUIRE((inputs.vertices.size() == kVertexCount * kLabelStride &&
             std::equal(inputs.vertices.begin(), inputs.vertices.end(), guest.at(kVertices))));
    UNSCOPED_INFO("texture bytes are the fetch's whole 1 MiB level");
    REQUIRE((inputs.textureBytes.size() == kTextureBytes && inputs.textureAddress == kTexture &&
             std::equal(inputs.textureBytes.begin(), inputs.textureBytes.end(), guest.physicalMemory.begin() + kTexture)));
    UNSCOPED_INFO("fetch0 words come from G+0x480");
    REQUIRE((inputs.textureFetch[0] == 0x08000002 && inputs.textureFetch[1] == (kTexture | 2) && inputs.textureFetch[2] == 0x007FE3FF));
    UNSCOPED_INFO("shader, fetch, declaration and state inputs are read");
    REQUIRE((inputs.vertexCode.size() == 48 && inputs.pixelCode == psCode && inputs.fetchRecords.size() == 12 &&
             inputs.fetchTemplates.size() == 36 && inputs.declaration.size() == 36 && inputs.swizzleTable.size() == 16 &&
             inputs.formatTable.size() == 64 && inputs.state.size() == 0x3500));
    UNSCOPED_INFO("fetch records follow the VS metadata");
    REQUIRE((inputs.fetchRecords[3] == 0 && word(inputs.fetchRecords, 4) == 0x1001));

    // A failure after the pixel shader matched is an error that keeps the raw inputs.
    guest.failTexture = true;
    UNSCOPED_INFO("an unreadable texture is a candidate failure");
    REQUIRE((!ReadLabelDrawInputsForDigest(digest, submission(), read, inputs, error) && !error.empty() && inputs.labelCandidate &&
             !inputs.inputsComplete && inputs.vertices.size() == kVertexCount * kLabelStride));
    guest.failTexture = false;
    UNSCOPED_INFO("label batch is admitted again");
    REQUIRE(ReadLabelDrawInputsForDigest(digest, submission(), read, inputs, error));

    // Bytes survive Save and Load, and a changed texture is refused.
    const auto root = std::filesystem::temp_directory_path() / ("label_draw_inputs_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    UNSCOPED_INFO("inputs save");
    REQUIRE(SaveLabelDrawInputs(root / "adapter", inputs, error));
    UNSCOPED_INFO("save needs a fresh directory");
    REQUIRE((!SaveLabelDrawInputs(root / "adapter", inputs, error) && error == "label_directory_not_fresh"));
    UNSCOPED_INFO("texture.bin holds 1 MiB");
    REQUIRE(std::filesystem::file_size(root / "adapter" / "texture.bin") == kTextureBytes);
    UNSCOPED_INFO("vertices.be.bin holds r5*28 bytes");
    REQUIRE(std::filesystem::file_size(root / "adapter" / "vertices.be.bin") == kVertexCount * kLabelStride);
    LabelDrawInputs loaded;
    UNSCOPED_INFO("inputs load");
    REQUIRE(LoadLabelDrawInputs(root / "adapter", loaded, error));
    UNSCOPED_INFO("scalar inputs round-trip");
    REQUIRE((loaded.arguments.graphics == inputs.arguments.graphics && loaded.arguments.vertexCount == inputs.arguments.vertexCount &&
             loaded.arguments.vertices == inputs.arguments.vertices && loaded.arguments.stride == inputs.arguments.stride &&
             loaded.arguments.caller == inputs.arguments.caller && loaded.vertexObject == inputs.vertexObject &&
             loaded.pixelObject == inputs.pixelObject && loaded.declarationAddress == inputs.declarationAddress &&
             loaded.textureAddress == inputs.textureAddress && loaded.textureFetch == inputs.textureFetch));
    UNSCOPED_INFO("vertex, texture, state and shader bytes round-trip");
    REQUIRE((loaded.vertices == inputs.vertices && loaded.textureBytes == inputs.textureBytes && loaded.state == inputs.state &&
             loaded.vertexCode == inputs.vertexCode && loaded.pixelCode == inputs.pixelCode &&
             loaded.vertexMetadata == inputs.vertexMetadata && loaded.pixelMetadata == inputs.pixelMetadata &&
             loaded.fetchRecords == inputs.fetchRecords && loaded.fetchTemplates == inputs.fetchTemplates &&
             loaded.declaration == inputs.declaration && loaded.declarationHeader == inputs.declarationHeader &&
             loaded.vertexHeader == inputs.vertexHeader && loaded.pixelHeader == inputs.pixelHeader &&
             loaded.swizzleTable == inputs.swizzleTable && loaded.formatTable == inputs.formatTable));
    UNSCOPED_INFO("a loaded fixture shader is not the retail pixel shader");
    REQUIRE(!loaded.labelCandidate);
    {
        std::fstream texture(root / "adapter" / "texture.bin", std::ios::in | std::ios::out | std::ios::binary);
        texture.seekp(4097);
        texture.put('\x55');
    }
    UNSCOPED_INFO("a changed texture byte fails the recorded digest");
    REQUIRE((!LoadLabelDrawInputs(root / "adapter", loaded, error) && error == "label_texture_digest"));

#if REREVVED_DEVELOPER
    // Capture: the label family claims the first candidate with no arm file
    // and no shader directory, and ends complete with the whole texture.
    diagnostics::LabelAdapterOptions options;
    options.read            = read;
    options.xenos           = false;
    options.pixelWordDigest = digest;
    const auto directory    = root / "capture";
    UNSCOPED_INFO("label capture starts");
    REQUIRE(diagnostics::StartLabelAdapterCapture(directory, options, error));
    UNSCOPED_INFO("no arm file is created or needed");
    REQUIRE(!std::filesystem::exists(directory / "arm"));
    UNSCOPED_INFO("one capture session at a time");
    REQUIRE(!diagnostics::StartLabelAdapterCapture(root / "second", options, error));
    auto other   = submission();
    other.caller = 0x82E3987C;
    UNSCOPED_INFO("another call site is not claimed");
    REQUIRE(!diagnostics::BeginLabelAdapterDraw(other));
    auto foreignTuple         = submission();
    foreignTuple.arguments[4] = 8;
    UNSCOPED_INFO("a foreign tuple is not claimed");
    REQUIRE(!diagnostics::BeginLabelAdapterDraw(foreignTuple));
    const auto capture = diagnostics::BeginLabelAdapterDraw(submission());
    UNSCOPED_INFO("the first label candidate is claimed");
    REQUIRE(capture != nullptr);
    UNSCOPED_INFO("claim saves the 1 MiB texture before Original");
    REQUIRE(std::filesystem::file_size(directory / "adapter" / "texture.bin") == kTextureBytes);
    UNSCOPED_INFO("a second candidate is not claimed");
    REQUIRE(!diagnostics::BeginLabelAdapterDraw(submission()));

    // Original: the CPU vertices are copied to the ring, the cursor is published
    // and the inline vertex shader copy is patched in place.
    std::copy(guest.at(kVertices), guest.at(kVertices) + kVertexCount * kLabelStride, guest.at(kRing));
    store(guest.virtualMemory, kGraphics - kBase + 0x3488, kRing);
    store(guest.virtualMemory, kGraphics - kBase + 0x3484, 0x1234);
    store(guest.virtualMemory, kGraphics - kBase + 0x30, 0x1234);
    for (unsigned i = 0; i < 48; ++i)
        guest.virtualMemory[kPatched - kBase + i] = static_cast<std::uint8_t>(0xA0 + i);
    diagnostics::ObserveLabelAdapterPatch(kVs, kPatched, kGraphics + 0x30F0, 0, 0x826ADB70);
    diagnostics::FinishLabelAdapterDraw(capture, kRing);
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("result.toml completes with the texture and no Xenos request");
    REQUIRE((result["complete"].value_or(false) && result["error"].value_or(std::string("x")).empty() &&
             result["texture_bytes"].value_or(0) == int(kTextureBytes) && result["vertex_count"].value_or(0) == int(kVertexCount) &&
             result["xenos_status"].value_or(std::string{}) == "disabled" && result["boundary"].value_or(std::int64_t{ 0 }) == 0x826A3000LL &&
             result["original_vs_source"].value_or(std::string{}) == "patched_inline_copy"));
    UNSCOPED_INFO("the observed patched vertex shader is saved");
    REQUIRE(readFile(directory / "original-vs.be.bin") == std::vector<std::uint8_t>(guest.at(kPatched), guest.at(kPatched) + 48));
    UNSCOPED_INFO("the ring copy of the vertices is saved");
    REQUIRE(readFile(directory / "original-vertices.be.bin") == inputs.vertices);
    UNSCOPED_INFO("texture.bin equals the guest texture");
    REQUIRE(readFile(directory / "adapter" / "texture.bin") == inputs.textureBytes);
    diagnostics::StopLabelAdapterCapture();
    UNSCOPED_INFO("stopping keeps a complete result");
    REQUIRE(toml::parse_file((directory / "result.toml").string())["complete"].value_or(false));

    // A draw that Original did not complete as the ABI says is a failed capture.
    UNSCOPED_INFO("second capture starts after Stop");
    REQUIRE(diagnostics::StartLabelAdapterCapture(root / "broken", options, error));
    const auto broken = diagnostics::BeginLabelAdapterDraw(submission());
    UNSCOPED_INFO("the batch is claimed again");
    REQUIRE(broken != nullptr);
    diagnostics::FinishLabelAdapterDraw(broken, kRing + 4);
    const auto failed = toml::parse_file((root / "broken" / "result.toml").string());
    UNSCOPED_INFO("a mismatched return fails the capture");
    REQUIRE((!failed["complete"].value_or(true) && !failed["error"].value_or(std::string{}).empty()));
    diagnostics::StopLabelAdapterCapture();

    // Stopping without a matching draw records why.
    UNSCOPED_INFO("third capture starts");
    REQUIRE(diagnostics::StartLabelAdapterCapture(root / "idle", options, error));
    diagnostics::StopLabelAdapterCapture();
    const auto idle = toml::parse_file((root / "idle" / "result.toml").string());
    UNSCOPED_INFO("an idle capture stops incomplete");
    REQUIRE((!idle["complete"].value_or(true) && idle["error"].value_or(std::string{}) == "label_capture_stopped_before_matching_draw"));

#endif

    std::filesystem::remove_all(root);
}
