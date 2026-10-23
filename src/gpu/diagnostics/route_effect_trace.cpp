#include "route_effect_trace.h"
#include "route_trace.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include <rex/crypto/sha256.h>
#include <rex/ppc/context.h>

namespace rerevved::gpu::diagnostics
{
namespace
{

// These are reachability labels from the route census, not semantic claims.
constexpr std::uint32_t readers[]{
    0x8268CAA8, 0x8268CBB8, 0x8268FB08, 0x8268FCB0, 0x82692948, 0x82692C48, 0x8269AF10, 0x8269B158, 0x8269B1F0, 0x8269B450, 0x8269BCA8, 0x8269BE70, 0x8269BF40, 0x8269C588, 0x8269C6C0, 0x8269C810, 0x8269C8E0, 0x8269D320, 0x8269E520, 0x826A3EB8, 0x826A3FB8, 0x826A4150, 0x826A4638, 0x826ADC48, 0x826ADD88, 0x82698BB0, 0x8269C688, 0x8269DB20, 0x826A0108, 0x826A57B8, 0x826A5B48, 0x826A8D60, 0x826A91B0
};

template <std::size_t N>
struct Encoding
{
    std::array<std::uint8_t, N> bytes{};
    std::size_t                 size = 0;

    void Add(std::uint64_t value, unsigned width) noexcept
    {
        for (unsigned i = 0; i < width; ++i)
            bytes[size++] = static_cast<std::uint8_t>(value >> (i * 8));
    }

    std::span<const std::uint8_t> Span() const noexcept
    {
        return { bytes.data(), size };
    }
};

constexpr std::size_t fieldIndex(std::uint32_t offset)
{
    for (std::size_t i = 0; i < std::size(kRouteEffectFields); ++i)
        if (kRouteEffectFields[i].offset == offset)
            return i;
    return std::size(kRouteEffectFields);
}

// Retained until process exit, matching the scene observer lifetime contract.
RouteEffectRecorder*                             owner = nullptr;
std::atomic<RouteEffectRecorder*>                current{ nullptr };
std::filesystem::path                            outputDirectory;
std::mutex                                       lifecycle;
std::string                                      finishError;
thread_local std::array<RouteEffectIdentity, 16> calls{};
thread_local std::size_t                         callDepth = 0;

bool same(const RouteEffectValue& a, const RouteEffectValue& b)
{
    return a.valid == b.valid && a.value == b.value;
}

} // namespace

RouteEffectRecorder::RouteEffectRecorder(RouteEffectRead reader, std::size_t budget)
: read(reader)
, bytes(std::min(budget, kRouteEffectBudget))
{
    if (!reader)
        throw std::invalid_argument("route_effect_reader_missing");
}

void RouteEffectRecorder::Emit(RouteEffectKind kind, RouteEffectIdentity identity, std::span<const std::uint8_t> payload) noexcept
{
    constexpr std::size_t headerSize = 48;
    const auto            ordinal    = ++sequence;
    if (sealed || headerSize + payload.size() > bytes.size() - used)
    {
        ++overflow;
        return;
    }
    Encoding<headerSize> header;
    header.Add(headerSize + payload.size(), 4);
    header.Add(static_cast<unsigned>(kind), 2);
    header.Add(0, 2);
    header.Add(ordinal, 8);
    header.Add(identity.frame, 8);
    header.Add(identity.call, 8);
    header.Add(identity.parent, 8);
    header.Add(identity.submission, 8);
    std::copy(header.bytes.begin(), header.bytes.end(), bytes.begin() + used);
    used += headerSize;
    std::copy(payload.begin(), payload.end(), bytes.begin() + used);
    used += payload.size();
}

RouteEffectValue RouteEffectRecorder::Read(std::uint64_t address, unsigned width) noexcept
{
    try
    {
        if (!address || address > UINT32_MAX || width > 0x100000000ULL - address)
            throw std::runtime_error("route_effect_extent");
        const auto data = read(address, width, false);
        if (data.size() != width)
            throw std::runtime_error("route_effect_short_read");
        std::uint64_t value = 0;
        for (const auto b : data)
            value = (value << 8) | b;
        return { value, true };
    }
    catch (...)
    {
        ++readFailures;
        return {};
    }
}

void RouteEffectRecorder::Gap(RouteEffectIdentity identity, RouteEffectIssue issue, std::uint64_t address, std::uint64_t size) noexcept
{
    ++gaps;
    Encoding<20> data;
    data.Add(static_cast<unsigned>(issue), 4);
    data.Add(address, 8);
    data.Add(size, 8);
    Emit(RouteEffectKind::Gap, identity, data.Span());
}

void RouteEffectRecorder::Window(bool enabled, std::uint64_t nextFrame) noexcept
{
    std::lock_guard lock(gate);
    for (auto& poll : polls)
        Flush(poll);
    frame  = nextFrame;
    active = enabled && !sealed;
}

RouteEffectToken RouteEffectRecorder::Begin(std::uint32_t entry, std::uint32_t graphics, std::uint32_t stack, std::uint32_t output, RouteEffectIdentity identity) noexcept
{
    std::lock_guard lock(gate);
    if (!active || !RouteEffectDraw(entry))
        return {};
    RouteEffectToken token;
    token.recorder = this;
    token.identity = identity;
    token.entry    = entry;
    token.graphics = graphics;
    if (callDepth == calls.size())
        ++overflow;
    else
        calls[callDepth] = identity;
    ++callDepth;
    // Preserve a change of submission even when progress values are unchanged.
    if (submission != identity.submission)
    {
        for (auto& poll : polls)
            Flush(poll);
        submission = identity.submission;
    }
    ++inflight;
    for (std::size_t i = 0; i < token.before.size(); ++i)
        token.before[i] = Read(std::uint64_t(graphics) + kRouteEffectFields[i].offset, kRouteEffectFields[i].width);
    if (entry == 0x826A3048)
    {
        // 826A3538/353C and 3544..3558: r10 and entry SP+84.
        token.outputs[0]  = output;
        const auto vertex = Read(std::uint64_t(stack) + 84, 4);
        if (!vertex.valid)
            Gap(identity, RouteEffectIssue::Read, std::uint64_t(stack) + 84, 4);
        token.outputs[1] = static_cast<std::uint32_t>(vertex.value);
        for (unsigned i = 0; i < 2; ++i)
            token.outputBefore[i] = Read(token.outputs[i], 4);
    }
    Encoding<16> data;
    data.Add(entry, 4);
    data.Add(graphics, 4);
    data.Add(token.outputs[0], 4);
    data.Add(token.outputs[1], 4);
    Emit(RouteEffectKind::Begin, identity, data.Span());
    return token;
}

bool RouteEffectRecorder::Field(const RouteEffectToken& token, std::uint64_t address, unsigned width, RouteEffectValue before, RouteEffectValue after) noexcept
{
    if (before.valid && after.valid && before.value == after.value)
    {
        ++unchangedFields;
        return false;
    }
    Encoding<32> data;
    data.Add(address, 8);
    data.Add(width, 4);
    data.Add((before.valid ? 1 : 0) | (after.valid ? 2 : 0), 4);
    data.Add(before.value, 8);
    data.Add(after.value, 8);
    Emit(RouteEffectKind::Field, token.identity, data.Span());
    return true;
}

void RouteEffectRecorder::Range(const RouteEffectToken& token, std::uint64_t address, std::uint64_t size, unsigned source) noexcept
{
    if (!size)
        return;
    // Hashing is bounded independently of metadata storage.
    if (!address || address > UINT32_MAX || size > 1024 * 1024 || size > 0x100000000ULL - address)
    {
        Gap(token.identity, RouteEffectIssue::Extent, address, size);
        return;
    }
    Encoding<56> data;
    data.Add(address, 8);
    data.Add(size, 8);
    data.Add(source, 4);
    RouteEffectIssue             issue = RouteEffectIssue::None;
    std::array<std::uint8_t, 32> hash{};
    try
    {
        const auto memory = read(address, size, false);
        if (memory.size() != size)
            throw std::runtime_error("route_effect_short_range");
        const auto sha = rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(memory.data()), memory.size()));
        if (sha.size() != 64)
            throw std::runtime_error("route_effect_digest");
        const auto nibble = [](char c)
        {
            return c <= '9' ? c - '0' : c - 'a' + 10;
        };
        for (std::size_t i = 0; i < hash.size(); ++i)
            hash[i] = static_cast<std::uint8_t>((nibble(sha[i * 2]) << 4) | nibble(sha[i * 2 + 1]));
    }
    catch (...)
    {
        ++readFailures;
        issue = RouteEffectIssue::Read;
    }
    data.Add(static_cast<unsigned>(issue), 4);
    for (const auto b : hash)
        data.Add(b, 1);
    Emit(RouteEffectKind::Range, token.identity, data.Span());
}

void RouteEffectRecorder::End(const RouteEffectToken& token, std::uint64_t result) noexcept
{
    if (token.recorder != this)
        return;
    std::lock_guard lock(gate);
    if (sealed)
    {
        if (callDepth)
            --callDepth;
        return;
    }
    std::array<RouteEffectValue, std::size(kRouteEffectFields)> after{};
    unsigned                                                    unchanged = 0;
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        const auto field = kRouteEffectFields[i];
        after[i]         = Read(std::uint64_t(token.graphics) + field.offset, field.width);
        unchanged += !Field(token, std::uint64_t(token.graphics) + field.offset, field.width, token.before[i], after[i]);
    }
    if (token.entry == 0x826A3048)
        for (unsigned i = 0; i < 2; ++i)
            unchanged += !Field(token, token.outputs[i], 4, token.outputBefore[i], Read(token.outputs[i], 4));

    const auto before = [&](std::uint32_t offset)
    {
        return token.before[fieldIndex(offset)];
    };
    const auto post = [&](std::uint32_t offset)
    {
        return after[fieldIndex(offset)];
    };
    const bool pending = (token.entry == 0x826A3048 && static_cast<std::uint32_t>(result) == 0) ||
                         (token.entry == 0x826A2B58 && static_cast<std::uint32_t>(result) != 0);
    const auto start = before(0x30), end = post(pending ? 0x3484 : 0x30);
    // stwu writes after the old cursor, through the new cursor inclusively.
    // A changed reservation boundary cannot establish intervening ring pieces.
    if (!start.valid || !end.valid)
        Gap(token.identity, RouteEffectIssue::Read, token.graphics + 0x30ULL, 4);
    else if (end.value < start.value || !same(before(0x34), post(0x34)) || !same(before(0x38), post(0x38)))
        Gap(token.identity, RouteEffectIssue::Relocated, start.value, end.value);
    else
        Range(token, start.value + 4, end.value - start.value, 1);

    const auto linkStart = before(0x33C8), linkEnd = post(0x33C8);
    if (!linkStart.valid || !linkEnd.valid)
        Gap(token.identity, RouteEffectIssue::Read, token.graphics + 0x33C8ULL, 4);
    else if (linkEnd.value < linkStart.value || linkEnd.value - linkStart.value > 4096 ||
             ((linkEnd.value - linkStart.value) & 15) || !same(before(0x33CC), post(0x33CC)))
        Gap(token.identity, RouteEffectIssue::Relocated, linkStart.value, linkEnd.value);
    else
        // 826A3D78 and analogous draw bodies write the first word of each 16-byte record.
        for (auto address = linkStart.value; address < linkEnd.value; address += 16)
            Range(token, address, 4, 2);

    Encoding<24> data;
    data.Add(token.entry, 4);
    data.Add(token.graphics, 4);
    data.Add(result, 8);
    data.Add(unchanged, 4);
    data.Add(after.size() + (token.entry == 0x826A3048 ? 2 : 0), 4);
    Emit(RouteEffectKind::End, token.identity, data.Span());
    if (inflight)
        --inflight;
    if (callDepth)
        --callDepth;
}

void RouteEffectRecorder::Copy(RouteEffectIdentity identity, std::uint32_t destination, std::uint32_t size) noexcept
{
    std::lock_guard lock(gate);
    if (sealed)
        return;
    RouteEffectToken token;
    token.identity = identity;
    Range(token, destination, size, 3);
}

void RouteEffectRecorder::Reader(std::uint32_t entry, std::uint64_t caller) noexcept
{
    const auto found = std::find(std::begin(readers), std::end(readers), entry);
    if (found == std::end(readers))
        return;
    std::lock_guard lock(gate);
    if (!active)
        return;
    const auto index = static_cast<unsigned>(found - std::begin(readers));
    if (readerMask & (1ULL << index))
        return;
    readerMask |= 1ULL << index;
    Encoding<16> data;
    data.Add(entry, 4);
    data.Add(index >= 25 ? 1 : 0, 4);
    data.Add(caller, 8);
    auto identity       = callDepth && callDepth <= calls.size() ? calls[callDepth - 1] : RouteEffectIdentity{};
    identity.frame      = frame;
    identity.submission = submission;
    Emit(RouteEffectKind::Reader, identity, data.Span());
    if (index >= 25)
        Gap(identity, RouteEffectIssue::Unobserved, entry, 0);
}

void RouteEffectRecorder::Flush(Poll& poll) noexcept
{
    if (!poll.used)
        return;
    Encoding<80> data;
    data.Add(poll.entry, 4);
    data.Add(poll.graphics, 4);
    data.Add(poll.state, 4);
    std::uint32_t valid = 0;
    for (unsigned i = 0; i < poll.values.size(); ++i)
        valid |= poll.values[i].valid ? 1U << i : 0;
    data.Add(valid, 4);
    data.Add(poll.count, 8);
    for (const auto v : poll.values)
        data.Add(v.value, 8);
    // Flush order can differ from observation order across polling owners.
    data.Add(poll.firstSequence, 8);
    data.Add(poll.samples, 8);
    Emit(RouteEffectKind::Progress, poll.identity, data.Span());
    poll = {};
}

void RouteEffectRecorder::Progress(std::uint32_t entry, std::uint32_t graphics, std::uint32_t pollingState, bool after) noexcept
{
    std::lock_guard lock(gate);
    if (!active)
        return;
    if (pollingState)
    {
        const auto g = Read(pollingState, 4);
        if (!g.valid)
        {
            Gap({ frame, 0, 0, submission }, RouteEffectIssue::Read, pollingState, 4);
            return;
        }
        graphics = static_cast<std::uint32_t>(g.value);
    }
    std::array<RouteEffectValue, 5> values{};
    values[0] = Read(std::uint64_t(graphics) + 0x2A90, 4);
    if (values[0].valid)
        values[1] = Read(values[0].value, 4);
    values[2] = Read(std::uint64_t(graphics) + 0x2A9C, 4);
    if (pollingState)
    {
        values[3] = Read(std::uint64_t(pollingState) + 8, 4);
        values[4] = Read(std::uint64_t(pollingState) + 12, 4);
    }
    Poll* slot = nullptr;
    for (auto& poll : polls)
    {
        if (poll.used && poll.entry == entry && poll.graphics == graphics && poll.state == pollingState)
        {
            slot = &poll;
            break;
        }
        if (!slot && !poll.used)
            slot = &poll;
    }
    if (!slot)
    {
        ++overflow;
        return;
    }
    if (slot->used && (slot->identity.frame != frame || slot->identity.submission != submission ||
                       !std::equal(values.begin(), values.end(), slot->values.begin(), same)))
        Flush(*slot);
    if (!slot->used)
    {
        slot->used          = true;
        slot->entry         = entry;
        slot->graphics      = graphics;
        slot->state         = pollingState;
        slot->identity      = { frame, 0, 0, submission };
        slot->values        = values;
        slot->firstSequence = ++sequence;
    }
    if (!after)
        ++slot->count;
    ++slot->samples;
}

void RouteEffectRecorder::Write(std::ostream& output)
{
    std::unique_lock lock(gate);
    active = false;
    if (!sealed)
    {
        for (auto& poll : polls)
            Flush(poll);
        if (inflight)
            Gap({ frame, 0, 0, submission }, RouteEffectIssue::Inflight, 0, inflight);
        sealed = true;
    }
    Encoding<64> header;
    for (const char c : std::string_view("RREVFX01"))
        header.Add(c, 1);
    header.Add(1, 4);
    header.Add(64, 4);
    header.Add(used, 8);
    header.Add(overflow, 8);
    header.Add(readFailures, 8);
    header.Add(gaps, 8);
    header.Add(inflight, 8);
    header.Add(readerMask, 8);
    // Sealing makes retained bytes immutable; disk I/O never holds the recorder gate.
    lock.unlock();
    output.write(reinterpret_cast<const char*>(header.bytes.data()), header.size);
    output.write(reinterpret_cast<const char*>(bytes.data()), used);
    if (!output)
        throw std::runtime_error("route_effect_write");
}

std::uint64_t RouteEffectRecorder::Overflow() const noexcept
{
    std::lock_guard lock(gate);
    return overflow;
}

std::uint64_t RouteEffectRecorder::ReadFailures() const noexcept
{
    std::lock_guard lock(gate);
    return readFailures;
}

RouteEffectStats RouteEffectRecorder::Stats() const noexcept
{
    std::lock_guard lock(gate);
    return { used, overflow, readFailures, gaps, inflight, unchangedFields };
}

bool StartRouteEffects(const std::filesystem::path& directory, RouteEffectRead read, std::string& error)
{
    std::lock_guard lock(lifecycle);
    try
    {
        if (owner)
            throw std::runtime_error("route_effect_session_already_started");
        outputDirectory = directory;
        owner           = new RouteEffectRecorder(read);
        current.store(owner, std::memory_order_release);
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
}

void SetRouteEffectWindow(bool active, std::uint64_t frame) noexcept
{
    if (auto* recorder = current.load(std::memory_order_acquire))
    {
        recorder->Window(active, frame);
        // Keep copy observers available while already admitted draw calls drain.
        // FinishRouteEffects restores count-only mode after the scene drain.
        if (active)
        {
            unsigned expected = 1;
            routeTraceEnabled.compare_exchange_strong(expected, kRouteTraceEffects, std::memory_order_release, std::memory_order_relaxed);
        }
    }
}

RouteEffectToken BeginRouteEffectDraw(std::uint32_t entry, const PPCContext& ctx, RouteEffectIdentity identity) noexcept
{
    if (auto* recorder = current.load(std::memory_order_acquire))
        return recorder->Begin(entry, ctx.r3.u32, ctx.r1.u32, ctx.r10.u32, identity);
    return {};
}

void EndRouteEffectDraw(const RouteEffectToken& token, const PPCContext& ctx) noexcept
{
    if (token.recorder)
        token.recorder->End(token, ctx.r3.u64);
}

RouteEffectReaderToken BeginRouteEffectReader(std::uint32_t entry, const PPCContext& ctx) noexcept
{
    RouteEffectReaderToken token;
    if (auto* recorder = current.load(std::memory_order_acquire))
    {
        recorder->Reader(entry, ctx.lr);
        if (entry == 0x827F8550 && callDepth && callDepth <= calls.size())
        {
            token.recorder    = recorder;
            token.entry       = entry;
            token.identity    = calls[callDepth - 1];
            token.destination = ctx.r3.u32;
            token.size        = ctx.r5.u32;
        }
        if (entry == 0x8269BE70 || entry == 0x8268CBB8)
        {
            token.recorder     = recorder;
            token.entry        = entry;
            token.graphics     = entry == 0x8269BE70 ? ctx.r3.u32 : 0;
            token.pollingState = entry == 0x8268CBB8 ? ctx.r3.u32 : 0;
            recorder->Progress(entry, token.graphics, token.pollingState);
        }
    }
    return token;
}

void EndRouteEffectReader(const RouteEffectReaderToken& token) noexcept
{
    if (token.recorder && token.entry == 0x827F8550)
        token.recorder->Copy(token.identity, token.destination, token.size);
    // Return samples retain final transitions without counting another iteration.
    if (token.recorder && (token.entry == 0x8269BE70 || token.entry == 0x8268CBB8))
        token.recorder->Progress(token.entry, token.graphics, token.pollingState, true);
}

bool FinishRouteEffects(std::string& error, RouteEffectStats* stats) noexcept
{
    std::lock_guard lock(lifecycle);
    try
    {
        auto*    recorder = current.exchange(nullptr, std::memory_order_acq_rel);
        unsigned expected = kRouteTraceEffects;
        routeTraceEnabled.compare_exchange_strong(expected, 1, std::memory_order_release, std::memory_order_relaxed);
        if (!recorder)
        {
            if (stats && owner)
                *stats = owner->Stats();
            error = finishError;
            return finishError.empty();
        }
        std::ofstream file(outputDirectory / "route-effects.pending", std::ios::binary | std::ios::noreplace);
        recorder->Write(file);
        if (stats)
            *stats = recorder->Stats();
        file.close();
        if (!file)
            throw std::runtime_error("route_effect_close");
        std::filesystem::rename(outputDirectory / "route-effects.pending", outputDirectory / "route-effects.bin");
        return true;
    }
    catch (const std::exception& e)
    {
        if (stats && owner)
            *stats = owner->Stats();
        error       = e.what();
        finishError = error;
        return false;
    }
}

} // namespace rerevved::gpu::diagnostics
