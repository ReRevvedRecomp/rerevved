#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct PPCContext;

namespace rerevved::gpu::diagnostics
{

using SceneFetch = std::array<std::uint32_t, 6>;

struct SceneTexture
{
    std::uint32_t type, base, mipBase, format, width, height, depth;
    std::uint32_t pitch, dimension, tiled, endian, swizzle, minMip, maxMip, packedMips;
    std::uint32_t signs, numFormat, stacked;
    std::int32_t  expAdjust;
};

// SDK include/rex/graphics/xenos.h: xe_gpu_texture_fetch_t. Words are host-order.
SceneTexture DecodeSceneFetch(const SceneFetch& words) noexcept;

struct SceneResolve
{
    std::uint32_t sourceIndex, primaryBase, format, width, height, layoutIncrement;
    std::uint32_t rawPitch, rawEndian;
};

// sub_8269E520 at 0x8269E5CC..0x8269E648; unnamed control fields stay raw.
SceneResolve DecodeSceneResolve(std::uint32_t flags, const SceneFetch& words) noexcept;

// Fetch-constant slots a shader's texture fetches name, from big-endian guest
// microcode. Throws when an exec clause runs past the code.
std::uint32_t SceneTextureFetchMask(std::span<const std::uint8_t> code);

struct ScenePacket
{
    std::uint64_t             sequence = 0, frame = 0;
    std::uint32_t             entry = 0;
    std::string               metadata;
    std::vector<std::uint8_t> state, vertexCode, pixelCode;
    std::size_t               Bytes() const noexcept;
};

// Bounded queue: only admission/pop/accounting are inside its mutex.
class SceneLog
{
public:
    explicit SceneLog(std::size_t capacity = 16384, std::size_t byteLimit = 512 * 1024 * 1024);
    bool                         Push(std::unique_ptr<ScenePacket> packet);
    std::unique_ptr<ScenePacket> Pop();

    std::uint64_t Overflow() const noexcept
    {
        return overflow.load(std::memory_order_relaxed);
    }

private:
    std::mutex                               mutex;
    std::deque<std::unique_ptr<ScenePacket>> queue;
    std::size_t                              capacity, byteLimit, bytes = 0;
    std::atomic<std::uint64_t>               overflow{ 0 };
};

struct SceneGuestRange
{
    std::uint32_t address = 0, size = 0;
};

// Live creation storage, merged into disjoint physical intervals for O(log N)
// copy membership. Rebuilds happen outside the reader's short critical section.
class SceneTextureStorage
{
public:
    bool Set(std::uint32_t object, std::vector<SceneGuestRange> ranges);
    void Release(std::uint32_t object);
    bool Overlaps(std::uint32_t address, std::uint32_t size) const;

private:
    void                                                            Rebuild();
    std::mutex                                                      updates;
    mutable std::shared_mutex                                       snapshotGate;
    std::unordered_map<std::uint32_t, std::vector<SceneGuestRange>> objects;
    std::vector<SceneGuestRange>                                    snapshot;
};

struct SceneWriterState;
struct ScenePayloadCall;
struct RouteEffectToken;

struct SceneHookToken
{
    void*                             session = nullptr;
    std::uint32_t                     entry = 0, object = 0;
    std::uint64_t                     parent = 0, caller = 0;
    std::shared_ptr<SceneWriterState> writer;
    bool                              suppressCopies = false;
    std::shared_ptr<ScenePayloadCall> payload;
    std::shared_ptr<RouteEffectToken> effect;
};

extern std::atomic<void*> sceneCaptureSession;
extern std::atomic<void*> sceneCoverageSession;

void ObserveSceneWriterRegion(void* session, bool completed, std::uint32_t stack, std::uint32_t record, std::uint32_t owner, std::uint32_t image, std::uint32_t conversion) noexcept;
void ObserveSceneAllocation(void* session, bool freed, std::uint32_t address, std::uint32_t bytes, std::uint32_t flags) noexcept;

constexpr bool SceneEntryObserved(std::uint32_t e) noexcept
{
    return e == 0x826A3568 || e == 0x826A3000 || e == 0x826A39F8 || e == 0x826A3608 || e == 0x826A3048 || e == 0x826A2B58 ||
           e == 0x82533DB8 || e == 0x8269AB90 || e == 0x82302DC8 || e == 0x826B7140 || e == 0x825184D0 ||
           e == 0x82304CF0 || e == 0x82578E00 || e == 0x8269E520 || e == 0x8268FB08 || e == 0x8268FCB0 ||
           e == 0x82518678 || e == 0x82697AA8 || e == 0x826A4638 || e == 0x8250AC90 ||
           e == 0x826B5F30 || e == 0x82521558 || e == 0x82305648 || e == 0x82C69850 ||
           e == 0x826B7E00 || e == 0x827F8550;
}

constexpr bool SceneEntryWindowOnly(std::uint32_t e) noexcept
{
    return e == 0x826A3568 || e == 0x826A3000 || e == 0x826A39F8 || e == 0x826A3608 || e == 0x826A3048 || e == 0x826A2B58 ||
           e == 0x8268FB08 || e == 0x8268FCB0 || e == 0x826A4638 || e == 0x8250AC90;
}

#if defined(REREVVED_SCENE_CAPTURE_TEST)
// Only the recorder test target replaces guest memory; production validates it.
using SceneTestMemoryReader = std::span<const std::uint8_t> (*)(std::uint64_t, std::uint64_t, bool);
void        SetSceneTestMemoryReader(SceneTestMemoryReader reader) noexcept;
void        SetSceneTestPreArmRegionBudget(void* session, std::uint64_t budget) noexcept;
bool        SceneTestWindowActive(void* session) noexcept;
bool        SceneTestArmed(void* session) noexcept;
std::size_t SceneTestWriterRangeCount(const SceneHookToken& token) noexcept;
#endif

SceneHookToken BeginSceneEntry(void* session, std::uint32_t entry, const PPCContext& ctx) noexcept;
void           EndSceneEntry(SceneHookToken token, const PPCContext& ctx) noexcept;

inline SceneHookToken ObserveSceneEntry(std::uint32_t entry, const PPCContext& ctx) noexcept
{
    if (auto* session = sceneCaptureSession.load(std::memory_order_relaxed))
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        return BeginSceneEntry(session, entry, ctx);
    }
    return {};
}

// Draw-triggered arming. The digest is the SHA-256 of a pixel shader's microcode with every
// 32-bit word byte-swapped to host order, the "word digest" the native shader catalogs pin.
std::string ScenePixelWordDigest(std::span<const std::uint8_t> guestMicrocode);

// Accepts 8 to 64 hex digits, any case, and returns them lower-cased. A prefix is enough to
// name a shader; the full digest is the unambiguous form.
bool NormalizeScenePixelDigest(std::string_view text, std::string& normalized);

// A watch fires once, from the guest thread that issues the first draw whose pixel shader word
// digest starts with the prefix. The callback must not block. The watch keeps the draw hooks
// observing even when no scene capture session is running.
using SceneDrawWatchHit = void (*)(void* context) noexcept;
bool AddSceneDrawWatch(std::string_view digestPrefix, SceneDrawWatchHit hit, void* context, std::string& error);
void RemoveSceneDrawWatches(void* context) noexcept;

struct SceneCaptureTriggers
{
    // Arm on the first draw whose pixel shader word digest starts with this prefix; empty keeps
    // the trigger off. The window opens armDelay + 1 boundaries after the frame holding that draw.
    std::string   pixelDigest;
    std::uint32_t armDelay = 0;
    // 1..16 sequential windows. Window 1 opens by file arm, arm_frame or digest and keeps the
    // capture directory; window k >= 2 opens when <directory>/arm-<k> exists and writes window-<k>/.
    std::uint32_t windows = 1;
};

bool StartSceneResourceCapture(const std::filesystem::path& directory, std::uint32_t frames, std::string_view titleRevision, std::string_view sdkRevision, std::string& error, bool payloads = false, std::uint32_t payloadBytes = 256 * 1024 * 1024, bool effects = false, bool targets = false, std::uint32_t targetBytes = 64 * 1024 * 1024, std::uint32_t armFrame = 0, const SceneCaptureTriggers& triggers = {});

void NotifySceneResourceFrameBoundary() noexcept;
void StopSceneResourceCapture() noexcept;

} // namespace rerevved::gpu::diagnostics
