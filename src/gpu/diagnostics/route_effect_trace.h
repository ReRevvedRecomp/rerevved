#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <span>
#include <string>
#include <vector>

struct PPCContext;

namespace rerevved::gpu::diagnostics
{

// Reuse the scene collector's checked reader; no guest pointers are retained.
using RouteEffectRead                           = std::span<const std::uint8_t> (*)(std::uint64_t, std::uint64_t, bool);
inline constexpr std::size_t kRouteEffectBudget = 10 * 1024 * 1024;

enum class RouteEffectKind : std::uint16_t
{
    Begin = 1,
    End,
    Field,
    Range,
    Reader,
    Progress,
    Gap
};

enum class RouteEffectIssue : std::uint32_t
{
    None       = 0,
    Read       = 1,
    Extent     = 2,
    Relocated  = 3,
    Inflight   = 4,
    Unobserved = 5
};

struct RouteEffectIdentity
{
    std::uint64_t frame = 0, call = 0, parent = 0, submission = 0;
};

struct RouteEffectValue
{
    std::uint64_t value = 0;
    bool          valid = false;
};

struct RouteEffectField
{
    std::uint32_t offset;
    std::uint8_t  width;
};

// Generated stores: 826A309C/3520..353C, 826A2FD8..2FE4,
// 826A3A44..3E1C, 826A3650..39CC, allocator 8269B898..B8E8.
inline constexpr RouteEffectField kRouteEffectFields[]{
    { 0x00, 8 }, { 0x08, 8 }, { 0x10, 8 }, { 0x18, 8 }, { 0x20, 8 }, { 0x30, 4 }, { 0x34, 4 }, { 0x38, 4 }, { 0x2ABD, 1 }, { 0x2E38, 1 }, { 0x30F0, 1 }, { 0x33B0, 4 }, { 0x33C0, 4 }, { 0x33C4, 4 }, { 0x33C8, 4 }, { 0x33CC, 4 }, { 0x3484, 4 }, { 0x3488, 4 }, { 0x348C, 4 }, { 0x3490, 4 }, { 0x3494, 4 }, { 0x34CC, 4 }, { 0x3A38, 4 }, { 0x3A3C, 4 }, { 0x3A40, 4 }, { 0x3A44, 4 }, { 0x3A48, 4 }, { 0x3A4C, 4 }, { 0x3A50, 4 }, { 0x3A54, 4 }, { 0x3A58, 4 }
};

class RouteEffectRecorder;

struct RouteEffectStats
{
    std::uint64_t bytes = 0, lost = 0, failedReads = 0, gaps = 0, unfinished = 0, unchangedFields = 0;
};

struct RouteEffectToken
{
    RouteEffectRecorder*                                        recorder = nullptr;
    RouteEffectIdentity                                         identity;
    std::uint32_t                                               entry = 0, graphics = 0;
    std::array<RouteEffectValue, std::size(kRouteEffectFields)> before{};
    std::array<std::uint32_t, 2>                                outputs{};
    std::array<RouteEffectValue, 2>                             outputBefore{};
};

// Fixed retained byte budget, fixed progress slots, and caller-owned tokens.
// Records are little-endian and contain no native structure padding.
class RouteEffectRecorder
{
public:
    explicit RouteEffectRecorder(RouteEffectRead read, std::size_t budget = kRouteEffectBudget);
    void             Window(bool active, std::uint64_t frame) noexcept;
    RouteEffectToken Begin(std::uint32_t entry, std::uint32_t graphics, std::uint32_t stack, std::uint32_t output, RouteEffectIdentity identity) noexcept;
    void             End(const RouteEffectToken& token, std::uint64_t result) noexcept;
    void             Reader(std::uint32_t entry, std::uint64_t caller) noexcept;
    void             Progress(std::uint32_t entry, std::uint32_t graphics, std::uint32_t pollingState = 0, bool after = false) noexcept;
    void             Copy(RouteEffectIdentity identity, std::uint32_t destination, std::uint32_t size) noexcept;
    void             Write(std::ostream& output);
    std::uint64_t    Overflow() const noexcept;
    std::uint64_t    ReadFailures() const noexcept;
    RouteEffectStats Stats() const noexcept;

private:
    struct Poll
    {
        bool                            used  = false;
        std::uint32_t                   entry = 0, graphics = 0, state = 0;
        RouteEffectIdentity             identity;
        std::array<RouteEffectValue, 5> values{};
        std::uint64_t                   count         = 0;
        std::uint64_t                   samples       = 0;
        std::uint64_t                   firstSequence = 0;
    };

    RouteEffectValue          Read(std::uint64_t address, unsigned width) noexcept;
    void                      Emit(RouteEffectKind kind, RouteEffectIdentity identity, std::span<const std::uint8_t> payload) noexcept;
    bool                      Field(const RouteEffectToken& token, std::uint64_t address, unsigned width, RouteEffectValue before, RouteEffectValue after) noexcept;
    void                      Range(const RouteEffectToken& token, std::uint64_t address, std::uint64_t size, unsigned source) noexcept;
    void                      Gap(RouteEffectIdentity identity, RouteEffectIssue issue, std::uint64_t address, std::uint64_t size) noexcept;
    void                      Flush(Poll& poll) noexcept;
    RouteEffectRead           read;
    mutable std::mutex        gate;
    std::vector<std::uint8_t> bytes;
    std::size_t               used  = 0;
    std::uint64_t             frame = 0, submission = 0, sequence = 0, overflow = 0, readFailures = 0, gaps = 0, inflight = 0;
    std::uint64_t             readerMask      = 0;
    std::uint64_t             unchangedFields = 0;
    bool                      active = false, sealed = false;
    std::array<Poll, 16>      polls{};
};

constexpr bool RouteEffectDraw(std::uint32_t e) noexcept
{
    return e == 0x826A3568 || e == 0x826A3000 || e == 0x826A3048 ||
           e == 0x826A2B58 || e == 0x826A39F8 || e == 0x826A3608;
}

// All activation comes from the scene session; there is no separate arm poll.
bool             StartRouteEffects(const std::filesystem::path& directory, RouteEffectRead read, std::string& error);
void             SetRouteEffectWindow(bool active, std::uint64_t frame) noexcept;
RouteEffectToken BeginRouteEffectDraw(std::uint32_t entry, const PPCContext& ctx, RouteEffectIdentity identity) noexcept;
void             EndRouteEffectDraw(const RouteEffectToken& token, const PPCContext& ctx) noexcept;
bool             FinishRouteEffects(std::string& error, RouteEffectStats* stats = nullptr) noexcept;

struct RouteEffectReaderToken
{
    RouteEffectRecorder* recorder = nullptr;
    std::uint32_t        entry = 0, graphics = 0, pollingState = 0;
    RouteEffectIdentity  identity;
    std::uint32_t        destination = 0, size = 0;
};

RouteEffectReaderToken BeginRouteEffectReader(std::uint32_t entry, const PPCContext& ctx) noexcept;
void                   EndRouteEffectReader(const RouteEffectReaderToken& token) noexcept;

} // namespace rerevved::gpu::diagnostics
