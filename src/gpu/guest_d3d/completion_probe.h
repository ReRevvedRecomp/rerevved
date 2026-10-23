#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

inline constexpr std::size_t kCompletionRecords = 128;
inline constexpr std::size_t kCompletionCalls   = 64;
inline constexpr std::size_t kCompletionDevices = 8;
// P, mask, B, S2, N, optional slots, manager, deferred queue, worker and CPU queues.
inline constexpr std::array<std::uint32_t, 29> kCompletionOffsets = {
    0x2A9C, 0x2AA0, 0x2A90, 0x2A94, 0x2B00, 0x347C, 0x409C, 0x40A0, 0x34CC, 0x34D0, 0x34D4, 0x34D8, 0x2AFC, 0x2B08, 0x2B0C, 0x2B04, 0x2C74, 0x2C78, 0x2CC4, 0x2CC8, 0x2D14, 0x2D18, 0x2D64, 0x2D68, 0x2DB4, 0x2DB8, 0x2E04, 0x2E08, 0x2AC0
};
inline constexpr std::size_t kCompletionWords = kCompletionOffsets.size() + 3;

using CompletionReader = bool (*)(void*, std::uint32_t, std::uint8_t*, std::size_t);
std::optional<std::uint32_t> ReadCompletionWord(bool enabled, CompletionReader reader, void* context, std::uint32_t base, std::uint32_t offset = 0) noexcept;

struct CompletionSnapshot
{
    std::array<std::uint32_t, kCompletionWords> words{};
    std::uint64_t                               valid      = 0;
    std::uint32_t                               unreadable = 0;

    bool Has(std::size_t index) const noexcept
    {
        return index < words.size() && (valid & (std::uint64_t{ 1 } << index)) != 0;
    }
};

CompletionSnapshot ReadCompletionSnapshot(CompletionReader reader, void* context, std::uint32_t device) noexcept;

enum class CompletionAdd : std::uint8_t
{
    Deferred,
    Direct,
    Stub
};
enum class CompletionBalance : std::uint8_t
{
    Ok,
    Bad,
    Indeterminate
};
std::uint32_t CompletionResidual(std::uint32_t before, std::uint32_t after, std::uint64_t additions, std::uint64_t retirements) noexcept;

struct CompletionCounts
{
    std::uint64_t                c810Enter = 0, c810Exit = 0, c810ExitNNonzero = 0;
    std::uint64_t                initSuccess = 0, initFailure = 0, dropped = 0;
    std::array<std::uint64_t, 3> adds{}, addCalls{};
    std::uint64_t                d = 0, balanceOk = 0, balanceBad = 0, balanceIndeterminate = 0;
    std::uint64_t                pOdd = 0, pEven = 0, pZero = 0, maskNonzero = 0, maskChange = 0;
    std::uint64_t                queueNonzero = 0, queueCursorChange = 0, queueSegmentChange = 0, s2Change = 0;
    std::array<std::uint64_t, 5> slotSamples{}, slotNonzero{}, slotChange{};
    std::uint64_t                callbacks = 0, extentHit = 0, extentOwnerJoined = 0;
    std::uint64_t                unreadable = 0, eventDrop = 0, pairError = 0, epochAmbiguous = 0, watchdogReset = 0;
    std::uint64_t                unmatched = 0;
};

struct CompletionRecord
{
    std::uint64_t                 sequence = 0, call = 0, previousCall = 0, epoch = 0;
    std::uint64_t                 additions = 0, retirements = 0;
    std::uint32_t                 device = 0, thread = 0, pc = 0;
    const char*                   kind = "sample";
    CompletionSnapshot            snapshot{};
    std::array<std::uint32_t, 12> payload{};
    std::uint32_t                 payloadValid = 0xFFF;
    CompletionBalance             balance      = CompletionBalance::Indeterminate;
};

struct CompletionReport
{
    CompletionCounts                                 counts{};
    std::array<CompletionRecord, kCompletionRecords> records{};
    std::size_t                                      size = 0;
};

// Fixed storage; Take drains reporting only. Event totals, calls and identities survive it.
class CompletionProbe
{
public:
    struct Stamp
    {
        std::uint64_t version = 0, quality = 0;
        std::uint32_t stores = 0;
    };

    Stamp            Sample();
    std::uint64_t    Begin(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, const CompletionSnapshot& snapshot, Stamp stamp, const std::array<std::uint32_t, 12>& payload = {}, std::uint32_t payloadValid = 0xFFF);
    void             End(std::uint64_t call, const CompletionSnapshot& snapshot, Stamp stamp, std::uint32_t result);
    void             Observe(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, const CompletionSnapshot& snapshot, const std::array<std::uint32_t, 12>& payload = {}, bool epoch = false, const char* kind = "sample");
    void             StoreBegin();
    void             StoreEnd(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, CompletionAdd path, std::uint32_t amount, std::optional<std::uint32_t> stored, bool retirement = false, const CompletionSnapshot& snapshot = {});
    void             Reset(std::uint32_t device);
    void             Failure();
    void             StoreCancel();
    void             Stubbed(std::uint64_t call);
    void             AllocationBegin(std::uint32_t thread, std::uint32_t pc, std::uint32_t device, std::uint32_t caller, std::optional<std::uint32_t> request, std::uint32_t alignment, std::uint32_t manager = 0, std::uint32_t flags = 0, std::uint32_t requestPointer = 0);
    void             AllocationTarget(std::uint32_t thread, std::uint32_t target, std::uint32_t context, std::optional<std::uint32_t> adjusted, const std::array<std::optional<std::uint32_t>, 3>& managerWords);
    void             AllocationEnd(std::uint32_t thread, std::uint32_t pc, std::uint32_t result, std::uint32_t physical, std::uint32_t regionBase, std::uint32_t regionSize);
    CompletionReport Take();

private:
    struct Boundary
    {
        CompletionSnapshot snapshot{};
        std::uint64_t      additions = 0, d = 0, epoch = 0, quality = 0, call = 0;
        bool               valid = false;
    };

    struct Device
    {
        std::uint32_t      address = 0;
        std::uint64_t      epoch = 0, additions = 0, d = 0, previousCall = 0;
        CompletionSnapshot previous{};
        Boundary           exit{};
    };

    struct Call
    {
        std::uint64_t id = 0;
        std::uint32_t pc = 0, device = 0, thread = 0;
        Boundary      start{};
        bool          uncertain = false, stubbed = false;
    };

    struct Allocation
    {
        bool                                        active = false;
        std::uint32_t                               thread = 0, pc = 0, device = 0, caller = 0, alignment = 0, manager = 0, flags = 0, target = 0, context = 0, requestPointer = 0;
        std::optional<std::uint32_t>                request, managerRequest;
        std::optional<std::uint32_t>                adjusted;
        std::array<std::optional<std::uint32_t>, 3> managerWords{};
        std::uint64_t                               id = 0;
    };

    Device*                                  FindDevice(std::uint32_t device);
    void                                     Record(CompletionRecord record);
    void                                     SampleFields(Device& device, CompletionRecord& record);
    void                                     Balance(Device& device, const Boundary& before, CompletionRecord& record, bool uncertain);
    void                                     Lost();
    std::mutex                               mutex_;
    CompletionReport                         report_{};
    std::array<Device, kCompletionDevices>   devices_{};
    Device                                   unknownDevice_{};
    std::array<Call, kCompletionCalls>       calls_{};
    std::array<Allocation, kCompletionCalls> allocations_{};

    struct ExtentIdentity
    {
        std::uint32_t device = 0, base = 0, size = 0;
        std::uint64_t epoch = 0;
        bool          used  = false;
    };

    std::array<ExtentIdentity, kCompletionCalls> extentGenerations_{};
    std::uint64_t                                sequence_ = 0, nextCall_ = 0, version_ = 0, quality_ = 0;
    std::uint32_t                                stores_                = 0;
    bool                                         allocationPairingLost_ = false;
};

extern std::atomic<std::int8_t> completionProbeState;
bool                            RefreshCompletionProbeRequested() noexcept;

inline bool CompletionProbeRequested() noexcept
{
    const auto state = completionProbeState.load(std::memory_order_relaxed);
    return state < 0 ? RefreshCompletionProbeRequested() : state != 0;
}

CompletionProbe& PublishedCompletionProbe() noexcept;
std::uint32_t    CompletionThread() noexcept;

// Only the listed route wrappers construct a scope, before any stub decision.
constexpr bool CompletionRouteEntry(std::uint32_t pc) noexcept
{
    return pc == 0x8269C810 || pc == 0x8269C8E0 || pc == 0x8269BCA8 || pc == 0x8269BF40 || pc == 0x826AC020;
}

class CompletionScope
{
public:
    CompletionScope(std::uint32_t pc, const PPCContext& context) noexcept
    : context_(context)
    , active_(CompletionProbeRequested())
    {
        if (active_) [[unlikely]]
            Begin(pc);
    }

    ~CompletionScope()
    {
        if (active_ && call_) [[unlikely]]
            End();
    }

    CompletionScope(const CompletionScope&)            = delete;
    CompletionScope& operator=(const CompletionScope&) = delete;
    void             Stubbed() const noexcept;

private:
    void              Begin(std::uint32_t pc) noexcept;
    void              End() noexcept;
    const PPCContext& context_;
    std::uint32_t     device_ = 0;
    std::uint64_t     call_   = 0;
    bool              active_ = false;
};

struct NoCompletionScope
{
    void Stubbed() const noexcept
    {
    }
};

template <std::uint32_t Address>
auto ObserveCompletionRoute(const PPCContext& context) noexcept
{
    if constexpr (CompletionRouteEntry(Address))
        return CompletionScope(Address, context);
    else
        return NoCompletionScope{};
}

void        ObserveCompletionSample(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, const std::array<std::uint32_t, 12>& payload = {}, bool epoch = false, const char* kind = "sample") noexcept;
void        ObserveCompletionStoreBegin(std::uint32_t pc) noexcept;
void        ObserveCompletionStoreEnd(std::uint32_t pc, std::uint32_t device, std::uint32_t thread, CompletionAdd path, std::uint32_t amount, std::uint32_t stored, bool retirement = false) noexcept;
void        ObserveCompletionStubBegin() noexcept;
void        ObserveCompletionStubEnd(std::uint32_t device, std::uint32_t thread, std::uint32_t amount, bool success) noexcept;
void        ObserveCompletionReset(std::uint32_t device) noexcept;
void        ObserveCompletionResetEnd() noexcept;
void        ObserveCompletionCallback(std::uint32_t pc, std::uint32_t device, std::uint32_t target, std::uint32_t argument, std::size_t words) noexcept;
void        ObserveCompletionAllocationBegin(std::uint32_t thread, std::uint32_t pc, std::uint32_t device, std::uint32_t caller, std::uint32_t request, std::uint32_t alignment, std::uint32_t manager = 0, std::uint32_t flags = 0) noexcept;
void        ObserveCompletionAllocationTarget(std::uint32_t thread, std::uint32_t target, std::uint32_t context, std::uint32_t requestPointer, std::uint32_t manager) noexcept;
void        ObserveCompletionAllocationEnd(std::uint32_t thread, std::uint32_t pc, std::uint32_t result) noexcept;
std::string FormatCompletionProbeCounts(const CompletionCounts& counts);
std::string ReportCompletionProbe(bool shutdown = false) noexcept;

} // namespace rerevved::gpu::guest_d3d
