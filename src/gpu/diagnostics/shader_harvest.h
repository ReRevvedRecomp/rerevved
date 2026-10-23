#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

struct IndexedBufferDrawInputs;

}

namespace rerevved::gpu::diagnostics
{

// One guest operation as its hook saw it: the hook address, the return
// address of the call (the caller), r3..r10 (r3 the device), r1 and, for the
// route-wrapped entries, r3 after Original.
struct ShaderHarvestCall
{
    std::uint32_t                entry = 0, caller = 0;
    std::array<std::uint32_t, 8> arguments{};
    std::uint32_t                stack = 0, result = 0;
    double                       depth = 0; // f1 at a clear
};

// Operations besides the six draw hooks. A clear is observed at 0x826A1D90
// entry; D3DDevice_Resolve's arguments at its entry and its copy words at
// 0x8269E8A0 (r31 = G); BeginTiling after it returns.
inline constexpr std::uint32_t kHarvestClearEntry       = 0x826A1D90;
inline constexpr std::uint32_t kHarvestResolveEntry     = 0x8269E520;
inline constexpr std::uint32_t kHarvestResolveCopyEntry = 0x8269E8A0;
inline constexpr std::uint32_t kHarvestBeginTilingEntry = 0x8269CE90;

// Set while a harvest runs. Draw hooks call it after Original returns, when
// the guest has applied its vertex fetch patch and state updates; the other
// operations at the sites above.
using ShaderHarvestObserver = void (*)(const ShaderHarvestCall& call) noexcept;
inline std::atomic<ShaderHarvestObserver> shaderHarvestObserver{ nullptr };
// Set with the observer; the VdSwap boundary hook calls it.
using ShaderHarvestBoundary = void (*)() noexcept;
inline std::atomic<ShaderHarvestBoundary> shaderHarvestBoundary{ nullptr };

struct ShaderHarvestOptions
{
    std::optional<std::uint32_t> caller;             // empty keeps every caller
    std::uint32_t                frameInstances = 0; // per new frame signature; 0 records no frame forms
    std::uint64_t                frameBytes     = 0; // budget per frame-form class
    // Signatures listed at most; frames of a further new signature are only counted.
    std::uint32_t frameSignatures = 4096;
    // Payload one open frame buffers; past it the frame keeps only its tokens.
    std::uint64_t frameBufferBytes = 128ULL << 20;
    // Prior harvests (see ShaderHarvest::Start). With any, a frame is written
    // only when it is new to them and to this run's written frames.
    std::vector<std::filesystem::path> known;
    // Frames are written only once this file exists; empty writes from the start.
    std::filesystem::path armFile;
};

// Draw key: caller/hook, shaders(pixel 0 if vertex-only), alpha, RT0 format,
// samples, primitive r4, target. Catalog vertex_hash/pixel_hash are XXH3-64 of
// big-endian microcode; *_word_sha256 names word digests.
struct ShaderHarvestPair
{
    std::uint32_t caller = 0, entry = 0;
    std::uint64_t vertexHash = 0, pixelHash = 0, keyHash = 0;
    std::uint32_t alphaTest = 0, colorFormat = 0, samples = 0, primitive = 0, variant = 0, textureMask = 0;
    bool          vertexOnly = false;
    std::string   targetForm, vertexWordDigest, pixelWordDigest, directory, error;
    std::string   knownHarvest; // set when `directory` is a known harvest's record
    std::uint64_t firstFrame = 0, draws = 0;
};

// One frame signature: the ordered non-draw operations of a frame, with the
// distinct draw tokens between each two of them as a sorted set.
struct ShaderHarvestFrameForm
{
    std::uint64_t            signature = 0, firstFrame = 0, frames = 0, markedFrames = 0, operations = 0;
    std::vector<std::string> instances, markedInstances;
    std::string              knownHarvest;        // a known harvest that wrote this signature
    bool                     writeFailed = false; // a write failed; the signature is not retried
};

// Coverage hashes non-draw tokens and draw segments; depth-only clear
// rectangles use a reduced class.
struct ShaderHarvestCoverage
{
    std::uint64_t                           shape = 0;
    std::vector<std::vector<std::uint64_t>> segments; // sorted, one per segment
};

ShaderHarvestCoverage ShaderHarvestCoverageOf(const std::vector<std::string>& canonical);

// Coverages of written frames. One covers a frame of the same shape whose
// draws in every segment are among its own at that segment.
class ShaderHarvestCoverageSet
{
public:
    bool Covers(const ShaderHarvestCoverage& coverage) const;
    void Add(const ShaderHarvestCoverage& coverage);

private:
    std::map<std::uint64_t, std::vector<std::vector<std::vector<std::uint64_t>>>> shapes;
};

// How a run's frames were handled, for WriteIndex and the replay below.
struct ShaderHarvestFrameCounts
{
    std::uint64_t known = 0, covered = 0, unarmed = 0, unlisted = 0, truncated = 0, failed = 0, bytes = 0, markedBytes = 0;
};

using ShaderHarvestRead = std::function<std::vector<std::uint8_t>(std::uint64_t, std::uint64_t, bool)>;

// First key writes vs_code.be.bin, optional ps_code.be.bin(vertex-only omits
// PS), post-Original state.be.bin, record.toml; index.toml preserves discovery
// order. New frame signatures go under frames/.
class ShaderHarvest
{
public:
    // `directory` must not exist; known harvests must be readable.
    bool Start(const std::filesystem::path& directory, const ShaderHarvestOptions& options, std::string& error);
    bool Selects(std::uint32_t caller) const noexcept;
    // Read operation through read; known written/covered signatures count
    // without rewriting. Schema>=3 known indexes lend existing key records
    // under their directories. Only existing instance/record files count.
    void Observe(const ShaderHarvestCall& call, const ShaderHarvestRead& read) noexcept;
    // Closes the frame: writes a new signature's frame, advances the frame
    // count and rewrites a changed index every 60 frames.
    void Boundary(const ShaderHarvestRead& read = {});
    bool WriteIndex(std::string& error);
    void Stop();

    std::vector<ShaderHarvestPair>      Pairs() const;
    std::vector<ShaderHarvestFrameForm> FrameForms() const;
    std::uint64_t                       Frame() const;
    ShaderHarvestFrameCounts            FrameCounts() const;
    // Admits one recorded frame form as a closed frame would be, charging
    // `bytes` per written instance without writing files; for replaying a
    // prior harvest's signatures against known harvests.
    void ReplayFrame(std::uint64_t signature, const std::vector<std::string>& canonical, bool marked, std::uint64_t bytes);
    // Storage capacity the open frame's operations hold.
    std::uint64_t RetainedFrameBytes() const;

private:
    struct Blob
    {
        std::string               role;
        std::uint64_t             address = 0, size = 0;
        bool                      physical = false, deferred = false;
        std::string               timing; // operation, next_operation or frame_end
        std::vector<std::uint8_t> bytes;
        std::string               error;
    };

    struct Operation
    {
        ShaderHarvestCall         call;
        std::string               kind, token, form;
        std::vector<std::uint8_t> state;
        std::size_t               pair        = SIZE_MAX;
        std::uint32_t             textureMask = 0;
        bool                      tiled       = false;
        std::vector<Blob>         blobs;
    };

    void        observeDraw(const ShaderHarvestCall& call, const ShaderHarvestRead& read);
    void        observeOther(const ShaderHarvestCall& call, const ShaderHarvestRead& read);
    std::size_t record(const ShaderHarvestCall& call, const ShaderHarvestPair& key, const std::string& keyText, const guest_d3d::IndexedBufferDrawInputs& inputs);
    void        skip(bool unreadable, const std::string& error);
    void        append(Operation operation, const ShaderHarvestRead& read);
    void        fillDeferred(const ShaderHarvestRead& read);
    void        closeFrame(const ShaderHarvestRead& read);
    // Lists, counts or writes a closed frame; `write` returns its bytes, or
    // nothing when the write failed.
    void        admitFrame(std::uint64_t signature, const std::vector<std::string>& canonical, const std::string& joined, bool marked, std::size_t operationCount, const std::function<std::optional<std::uint64_t>(const std::string&)>& write);
    bool        writeFrame(const std::string& name, std::uint64_t signature, bool marked, const std::vector<std::string>& canonical, const ShaderHarvestRead& read, std::uint64_t& bytes);
    std::string writeBlob(const std::vector<std::uint8_t>& bytes, std::uint64_t& written);
    bool        writeIndexLocked(std::string& error);
    void        loadKnown(const std::filesystem::path& directory, std::set<std::filesystem::path>& loading);
    bool        armedLocked();

    struct KnownKey
    {
        std::string directory, harvest;
    };

    mutable std::mutex                           mutex;
    std::filesystem::path                        root;
    ShaderHarvestOptions                         options;
    std::map<std::string, std::size_t>           known;
    std::vector<ShaderHarvestPair>               pairs;
    std::map<std::uint32_t, std::uint64_t>       entryDraws;
    std::uint64_t                                frame = 0, draws = 0, withoutShaders = 0, unreadable = 0, failures = 0, indexedFrame = 0;
    std::string                                  firstError;
    bool                                         running = false, dirty = false;
    std::map<std::thread::id, ShaderHarvestCall> resolveEntries;
    // The open frame and the frame forms seen so far.
    std::vector<Operation>                          operations;
    std::vector<std::size_t>                        deferredOperations;
    std::uint64_t                                   buffered  = 0;
    bool                                            truncated = false;
    std::map<std::uint64_t, ShaderHarvestFrameForm> forms;
    std::vector<std::uint64_t>                      formOrder;
    std::set<std::uint64_t>                         writtenBlobs;
    std::uint64_t                                   frameBytes = 0, markedBytes = 0, truncatedFrames = 0, unlistedFrames = 0;
    // Known harvests and the coverage of every written frame. Forms listed
    // once armed, and novel forms past the signature cap, are each bounded
    // by the cap.
    std::map<std::uint64_t, KnownKey>    knownKeys;
    std::map<std::uint64_t, std::string> knownSignatures, knownMarked;
    ShaderHarvestCoverageSet             coverage;
    std::uint64_t                        knownFrames = 0, coveredFrames = 0, unarmedFrames = 0, failedFrames = 0, novelForms = 0, armedForms = 0;
    std::optional<std::uint64_t>         armedFrame;
};

// Observes one operation of the process harvest.
void HarvestOperation(ShaderHarvest& harvest, const ShaderHarvestCall& call, const ShaderHarvestRead& read) noexcept;

// Parses an exact 0x-prefixed 32-bit hex caller address.
bool ParseShaderHarvestCaller(const std::string& text, std::uint32_t& caller);

// The process harvest over guest memory, installed as shaderHarvestObserver.
bool StartShaderHarvest(const std::filesystem::path& directory, const ShaderHarvestOptions& options, ShaderHarvestRead read, std::string& error);
void StopShaderHarvest() noexcept;

} // namespace rerevved::gpu::diagnostics
