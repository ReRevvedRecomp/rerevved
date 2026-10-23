

#include "gpu/guest_d3d/lowerer_derive.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kSp = 0x70000000, kVertexOut = 0x40020010, kIndexOut = 0x40020020, kPoolObject = 0x40030000, kPoolBase = 0x40100000, kPoolSize = 0x00040000,
                        kProgressBlock = 0x40040000;
constexpr std::uint32_t kRing          = 0x40080000;

using Memory = std::map<std::uint32_t, std::uint32_t>;

// The device words the model watches, as a test sets them.
struct Device
{
    std::array<std::uint64_t, 5> dirty{};
    std::uint64_t                mask   = 0;
    std::uint32_t                cursor = kRing + 0x100, limit34 = kRing + 0x40000, limit38 = kRing + 0x3F000, fence = 7;
    std::uint8_t                 tilingByte = 0, allocByte = 0, stateByte = 0;
    std::uint32_t                strideWord   = 0x08000000; // recorded stride byte 8 (an entry stride of 32 bytes)
    std::uint32_t                vertexShader = 0x40500000, targetCount = 2;
    std::uint32_t                bracket = 0, bracketNext = 0;
    std::uint32_t                tileChunk = 0x40600000, tileCursor = 0x40600000, tileLimit = 0x40600100;
    std::uint32_t                staging[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 }; // G+0x3484..0x3494
    std::uint32_t                poolObject = kPoolObject, poolBase = kPoolBase, poolSize = kPoolSize;
    std::uint32_t                poolPos = 0, wrapPos = 0, poolUsed = 0, threshold = 0x100000;
    std::uint32_t                sideEnd = 0, wrapLimit = 0, segment = 0, progress = 0;
    std::uint32_t                vertexOutWord = 0xAAAA0001, indexOutWord = 0xAAAA0002;
};

LowererEntry entryOf(std::uint32_t vertices = 100, std::uint32_t indices = 301, std::uint32_t flags = 0, std::uint32_t stride = 32)
{
    return { kG, vertices, indices, flags, stride, kIndexOut, kSp };
}

Memory build(const Device& d, const LowererEntry& e)
{
    Memory m;
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        m[kG + 8 * i]     = static_cast<std::uint32_t>(d.dirty[i] >> 32);
        m[kG + 8 * i + 4] = static_cast<std::uint32_t>(d.dirty[i]);
    }
    m[kG + 0x28]   = static_cast<std::uint32_t>(d.mask >> 32);
    m[kG + 0x2C]   = static_cast<std::uint32_t>(d.mask);
    m[kG + 0x30]   = d.cursor;
    m[kG + 0x34]   = d.limit34;
    m[kG + 0x38]   = d.limit38;
    m[kG + 0x2A9C] = d.fence;
    m[kG + 0x2ABC] = static_cast<std::uint32_t>(d.tilingByte) << 24 | static_cast<std::uint32_t>(d.allocByte) << 16 | static_cast<std::uint32_t>(d.stateByte) << 8;
    m[kG + 0x2E38] = d.strideWord;
    m[kG + 0x3198] = d.vertexShader;
    m[kG + 0x31CC] = d.targetCount;
    m[kG + 0x33B0] = d.bracket;
    m[kG + 0x33B4] = d.bracketNext;
    m[kG + 0x33C4] = d.tileChunk;
    m[kG + 0x33C8] = d.tileCursor;
    m[kG + 0x33CC] = d.tileLimit;
    for (std::uint32_t i = 0; i < 5; ++i)
        m[kG + 0x3484 + 4 * i] = d.staging[i];
    m[kG + 0x34CC]        = d.poolObject;
    m[kG + 0x3A44]        = d.poolPos;
    m[kG + 0x3A4C]        = d.wrapPos;
    m[kG + 0x3A54]        = d.poolUsed;
    m[kG + 0x3A58]        = d.threshold;
    m[kG + 0x3A3C]        = d.sideEnd;
    m[kG + 0x3A50]        = d.wrapLimit;
    m[kG + 0x3A48]        = d.segment;
    m[kG + 0x2A90]        = kProgressBlock;
    m[kProgressBlock + 4] = d.progress;
    m[kPoolObject + 152]  = d.poolBase;
    m[kPoolObject + 156]  = d.poolSize;
    m[e.stack + 84]       = kVertexOut;
    m[kVertexOut]         = d.vertexOutWord;
    m[e.indexOut]         = d.indexOutWord;
    return m;
}

GuestWordReader readerOf(const Memory& m)
{
    return [&m](std::uint32_t address, std::uint32_t& value)
    {
        const auto found = m.find(address);
        if (found == m.end())
            return false;
        value = found->second;
        return true;
    };
}

LowererWatch watchOf(const Device& d, const LowererEntry& e)
{
    const auto   m = build(d, e);
    LowererWatch w;
    UNSCOPED_INFO("watch unreadable");
    REQUIRE(ReadLowererWatch(readerOf(m), e, w));
    return w;
}

// ---- The literal emulation of Original's success path over a Memory. ----

// The words one flush helper emits, by walking the mask as the helper does: the
// run of set bits from the top (cntlzd), the run length from the complement.
std::vector<unsigned> runsFromTop(std::uint64_t mask)
{
    std::vector<unsigned> runs;
    while (mask)
    {
        const unsigned lead = static_cast<unsigned>(__builtin_clzll(mask));
        mask <<= lead;
        const std::uint64_t inverse = ~mask;
        const unsigned      length  = inverse ? static_cast<unsigned>(__builtin_clzll(inverse)) : 64;
        runs.push_back(length);
        mask = length >= 64 ? 0 : mask << length;
    }
    return runs;
}

// 0x826ACFF8 uses cursor-nibble filler, header and 16 words per bit
// (g169:12999-13136); full dirty-mask walk: g169:12940-13149.
unsigned emulateAcff8(std::uint64_t mask, std::uint32_t& cursor)
{
    unsigned words = 0;
    for (const auto n : runsFromTop(mask))
    {
        const auto low    = cursor & 0xF;
        const auto filler = low == 4 ? 1U : low == 0 ? 2U
                                        : low == 12  ? 3U
                                                     : 0U;
        words += filler + 1 + 16 * n;
        cursor += 4 * (filler + 1 + 16 * n);
    }
    return words;
}

// 0x826ACEA0 (g92:13099-13297): a header (and a filler when the cursor is 4 past a doubleword), 6 words per bit, 4 closing words.
unsigned emulateAcea0(std::uint64_t mask, std::uint32_t& cursor)
{
    unsigned words = 0;
    for (const auto n : runsFromTop(mask))
    {
        const unsigned lead = (cursor & 7) ? 2U : 1U;
        words += lead + 6 * n;
        cursor += 4 * (lead + 6 * n);
    }
    words += 4;
    cursor += 16;
    return words;
}

// 0x826ACC60 (g71:12144-12260): a header and a word per bit.
unsigned emulateAcc60(std::uint64_t mask, std::uint32_t& cursor)
{
    unsigned words = 0;
    for (const auto n : runsFromTop(mask))
    {
        words += 1 + n;
        cursor += 4 * (1 + n);
    }
    return words;
}

// 0x826ACD20 (g209:13175-13410): 13 words, then a header (and filler) and 4 words per bit.
unsigned emulateAcd20(std::uint64_t mask, std::uint32_t& cursor)
{
    unsigned words = 13;
    cursor += 52;
    for (const auto n : runsFromTop(mask))
    {
        const unsigned lead = (cursor & 7) ? 2U : 1U;
        words += lead + 4 * n;
        cursor += 4 * (lead + 4 * n);
    }
    return words;
}

std::uint64_t bits(unsigned low, unsigned count)
{
    return (count >= 64 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << count) - 1) << low;
}

// The ring words Original's flush groups emit for these dirty words at this cursor (g63:13109-13410).
unsigned emulateFlush(const Device& d, std::uint32_t cursor, std::uint32_t& endCursor)
{
    unsigned   words = 0;
    const auto top   = [](std::uint64_t value, unsigned low, unsigned count)
    {
        return (value & bits(low, count));
    };
    if (d.dirty[0])
        words += emulateAcff8(d.dirty[0], cursor);
    if (d.dirty[1])
        words += emulateAcff8(d.dirty[1], cursor);
    if (d.dirty[2])
    {
        if (d.mask & d.dirty[2])
        {
            // 0x826AC8C8: at most 32 + 5n words (g202:13191-13594); the emulation takes the bound.
            words += 32 + 5 * d.targetCount;
            cursor += 4 * (32 + 5 * d.targetCount);
        }
        for (const auto [low, count] : { std::pair{ 0U, 12U }, std::pair{ 12U, 5U }, std::pair{ 42U, 16U }, std::pair{ 21U, 21U } })
            if (top(d.dirty[2], low, count))
                words += emulateAcc60(top(d.dirty[2], low, count), cursor);
    }
    if (d.dirty[3])
    {
        if (d.dirty[3] & 0xFFFFFFFFULL)
            words += emulateAcea0(d.dirty[3] & 0xFFFFFFFFULL, cursor);
        if (top(d.dirty[3], 34, 21))
            words += emulateAcc60(top(d.dirty[3], 34, 21), cursor);
    }
    if (d.dirty[4])
    {
        for (const auto [low, count] : { std::pair{ 0U, 38U }, std::pair{ 38U, 8U } })
            if (top(d.dirty[4], low, count))
                words += emulateAcc60(top(d.dirty[4], low, count), cursor);
        if (top(d.dirty[4], 49, 6))
            words += emulateAcd20(top(d.dirty[4], 49, 6), cursor);
        if ((d.dirty[4] >> 56) & 1)
        {
            words += emulateAcc60(bits(24, 40), cursor);
        }
    }
    endCursor = cursor;
    return words;
}

struct Emulated
{
    bool          ok      = false; // false when Original would flush, wait or fail
    std::uint32_t limit34 = 0, limit38 = 0, poolPos = 0, poolUsed = 0, wrapPos = 0;
    std::uint32_t vertexDest = 0, indexDest = 0, vertexDwords = 0, indexDwords = 0;
    unsigned      flushWords = 0;
};

// 0x826A6848 with r4 = 1 (g100:13237-13311) on a copy of the device words.
std::uint32_t emulateFixedTake(Memory& m, std::uint32_t bytesAddressValue, std::uint32_t align)
{
    const std::uint32_t pool = m[kG + 0x34CC];
    std::uint32_t       top  = m[kG + 0x3A44];
    if (!top)
        top = (m[pool + 156] & 0xFFFFFFFC) + m[pool + 152];
    const std::uint32_t size   = bytesAddressValue & 0xFFFFFFFC;
    const std::uint32_t buffer = (top - size) & ~(align - 1);
    if (buffer < m[kG + 0x30] + 164)
        return 0;
    const std::uint32_t delta = (top - buffer) & 0xFFFFFFFC;
    m[kG + 0x3A44]            = buffer;
    m[kG + 0x34] -= delta;
    m[kG + 0x38] -= delta;
    return buffer;
}

// 0x8269B618 with r4 = 1 (g185:12921-13040); false on the failure returns or when 0x8269B158 would wait.
bool emulateNullTake(Memory& m, std::uint32_t size, std::uint32_t align, std::uint32_t& out)
{
    std::uint32_t r11 = m[kG + 0x3A44];
    if (!r11)
        r11 = m[kG + 0x34] + 60;
    const std::uint32_t segment = m[kG + 0x3A48], sideEnd = m[kG + 0x3A3C], bracket = m[kG + 0x33B0], next = m[kG + 0x33B4];
    std::uint32_t       buffer = (r11 + align - 1) & ~(align - 1);
    std::uint32_t       end    = size + buffer;
    std::uint32_t       seg    = segment;
    if (end <= sideEnd)
    {
        if (bracket)
        {
            const std::uint32_t diff = next - segment;
            if (diff == 0xFFFFFFFF)
                return false;
            if (diff == 0 && bracket < end)
                return false;
        }
        m[kG + 0x3A44] = end;
    }
    else
    {
        buffer = (m[kG + 0x3A4C] + align - 1) & ~(align - 1);
        end    = size + buffer;
        if (bracket)
        {
            const std::uint32_t diff = next - segment - 1;
            if (diff == 0xFFFFFFFF)
                return false;
            if (diff == 0 && bracket < end)
                return false;
        }
        if (end > m[kG + 0x3A50])
            return false;
        seg += 1;
        m[kG + 0x3A4C] = end;
    }
    // 0x8269B158: no wait when the GPU's segment is this one, or one behind with the buffer inside its block (g206:12440-12459).
    const std::uint32_t progress = m[kProgressBlock + 4];
    const std::uint32_t ahead    = (seg - progress) & 3;
    if (ahead != 0 && !(ahead == 1 && end <= (progress & 0xFFFFFFFC)))
        return false;
    out = buffer;
    return true;
}

// Literal 0x826A3048 oracle, independent of lowerer_derive.cpp.
Emulated emulate(const Device& d, const LowererEntry& e)
{
    Emulated out;
    Memory   m = build(d, e);
    // g63:13055-13081
    const std::int64_t  product = static_cast<std::int64_t>(static_cast<std::int32_t>(e.vertexCount)) * static_cast<std::int32_t>(e.stride);
    const std::uint32_t r19     = (static_cast<std::uint32_t>(product) >> 2) & 0x3FFFFFFF;
    const std::uint32_t r18     = (e.flags & 4) ? e.indexCount : ((e.indexCount + 1) >> 1) & 0x7FFFFFFF;
    std::uint32_t       end     = 0;
    out.flushWords              = emulateFlush(d, d.cursor, end);
    m[kG + 0x30]                = end;
    // The soft limit flush (g63:13414-13425) and the helpers' own limit tests are Original's flush paths.
    if (end > d.limit38 || end >= d.limit34)
        return out;
    std::uint32_t       used = d.poolUsed;
    std::uint32_t       buffers[2]{};
    const std::uint32_t dwords[2] = { r19, r18 };
    // Allocator 0x8269B818 (g93:12757-12888) takes vertex/index buffers.
    for (int i = 0; i < 2; ++i)
    {
        const std::uint32_t bytes = dwords[i] << 2; // g93:12787-12793
        if (m[kG + 0x34CC])
            buffers[i] = emulateFixedTake(m, bytes, 16);
        else if (!emulateNullTake(m, bytes, 16, buffers[i]))
            return out;
        if (!buffers[i])
            return out;
        used += bytes;
        // g93:12837-12865: past the threshold with the bracket clear the limits move to the cursor.
        if (used > d.threshold && m[kG + 0x33B0] == 0)
            return out;
    }
    // Success epilogue: g63:13772-13795.
    out.ok           = true;
    out.limit34      = m[kG + 0x34];
    out.limit38      = m[kG + 0x38];
    out.poolPos      = m[kG + 0x3A44];
    out.wrapPos      = m[kG + 0x3A4C];
    out.poolUsed     = used;
    out.vertexDest   = buffers[0];
    out.indexDest    = buffers[1];
    out.vertexDwords = r19;
    out.indexDwords  = r18;
    return out;
}

void testDraw()
{
    // Dwords of the vertices: the low 32 bits of the signed product, shifted right two (g63:13055, 13070).
    {
        const auto draw = DeriveLowererDraw(entryOf(100, 301, 0, 32));
        UNSCOPED_INFO("vertex dwords");
        REQUIRE((draw.vertexDwords == 800 && draw.vertexBytes == 3200));
        UNSCOPED_INFO("16-bit index dwords round up");
        REQUIRE((draw.indexDwords == 151 && draw.indexBytes == 604 && !draw.indices32));
        UNSCOPED_INFO("stride byte");
        REQUIRE(draw.strideByte == 8);
    }
    {
        const auto draw = DeriveLowererDraw(entryOf(100, 301, 4, 32));
        UNSCOPED_INFO("32-bit index dwords");
        REQUIRE((draw.indices32 && draw.indexDwords == 301));
    }
    {
        // 0x80000000 * 4 overflows 32 bits to zero; a negative count wraps by the signed product's low bits.
        UNSCOPED_INFO("product low 32 bits");
        REQUIRE(DeriveLowererDraw(entryOf(0x40000000, 2, 0, 4)).vertexDwords == 0);
        const auto draw = DeriveLowererDraw(entryOf(0xFFFFFFFF, 2, 0, 8));
        UNSCOPED_INFO("negative product");
        REQUIRE(draw.vertexDwords == (static_cast<std::uint32_t>(-8) >> 2));
        const auto odd = DeriveLowererDraw(entryOf(1, 0, 0, 36));
        UNSCOPED_INFO("empty indices");
        REQUIRE((odd.indexDwords == 0 && odd.strideByte == 9));
    }
}

void testRead()
{
    const Device d{};
    const auto   e = entryOf();
    LowererWatch w = watchOf(d, e);
    UNSCOPED_INFO("ring words");
    REQUIRE((w.cursor == d.cursor && w.limit34 == d.limit34 && w.limit38 == d.limit38));
    UNSCOPED_INFO("state words");
    REQUIRE((w.fence == 7 && w.targetCount == 2 && w.vertexShader == d.vertexShader && w.strideWord == d.strideWord));
    UNSCOPED_INFO("out words");
    REQUIRE((w.vertexOutAddress == kVertexOut && w.vertexOut == d.vertexOutWord && w.indexOut == d.indexOutWord));
    UNSCOPED_INFO("pool words");
    REQUIRE((w.poolObject == kPoolObject && w.poolBase == kPoolBase && w.poolSize == kPoolSize && w.poolThreshold == d.threshold));
    UNSCOPED_INFO("staging words");
    REQUIRE((w.pendingEnd == 0x11 && w.vertexDest == 0x22 && w.indexDest == 0x33 && w.vertexDwords == 0x44 && w.indexDwords == 0x55));
    UNSCOPED_INFO("tile list words");
    REQUIRE((w.tileChunk == d.tileChunk && w.tileCursor == d.tileCursor && w.tileLimit == d.tileLimit));

    // The tiling, alloc and state bytes sit in the three high bytes of G+0x2ABC.
    Device bytes{};
    bytes.tilingByte = 0x21;
    bytes.allocByte  = 0x20;
    bytes.stateByte  = 0x7E;
    w                = watchOf(bytes, e);
    UNSCOPED_INFO("state bytes");
    REQUIRE((w.tilingByte == 0x21 && w.allocByte == 0x20 && w.stateByte == 0x7E));

    // The null pool allocator's words are read only without a pool object.
    Device nopool{};
    nopool.poolObject = 0;
    nopool.sideEnd    = 0x40800000;
    nopool.wrapLimit  = 0x40900000;
    nopool.segment    = 3;
    nopool.progress   = 5;
    w                 = watchOf(nopool, e);
    UNSCOPED_INFO("null pool words");
    REQUIRE((w.poolObject == 0 && w.sideEnd == 0x40800000 && w.wrapLimit == 0x40900000 && w.segment == 3 && w.progress == 5));

    // Unreadable words fail the read: each of the memory words in turn, except the
    // words the other pool shape reads.
    for (const bool withPool : { true, false })
    {
        Device shape{};
        shape.poolObject  = withPool ? kPoolObject : 0;
        const auto memory = build(shape, e);
        for (const auto& [address, value] : memory)
        {
            const bool poolOnly = address == kPoolObject + 152 || address == kPoolObject + 156;
            const bool nullOnly = address == kG + 0x3A3C || address == kG + 0x3A50 || address == kG + 0x3A48 || address == kG + 0x2A90 || address == kProgressBlock + 4;
            if (withPool ? nullOnly : poolOnly)
                continue;
            Memory copy = memory;
            copy.erase(address);
            LowererWatch scratch;
            UNSCOPED_INFO(std::format("a missing word {:08X} must fail the read", address));
            REQUIRE(!ReadLowererWatch(readerOf(copy), e, scratch));
        }
    }
    // An out word address of zero, a null index out and a wrapping frame are refused.
    Device zero{};
    auto   zeroMemory        = build(zero, e);
    zeroMemory[e.stack + 84] = 0;
    LowererWatch scratch;
    UNSCOPED_INFO("a null vertex out address");
    REQUIRE(!ReadLowererWatch(readerOf(zeroMemory), e, scratch));
    auto bad     = e;
    bad.indexOut = 0;
    UNSCOPED_INFO("a null index out address");
    REQUIRE(!ReadLowererWatch(readerOf(build(zero, e)), bad, scratch));
    bad       = e;
    bad.stack = 0xFFFFFFF0;
    UNSCOPED_INFO("a frame at the end of memory");
    REQUIRE(!ReadLowererWatch(readerOf(build(zero, e)), bad, scratch));
}

void testFlushWords()
{
    LowererWatch w;
    UNSCOPED_INFO("no dirty bits");
    REQUIRE(LowererFlushWords(w) == 0);

    // Hand-computed single groups.
    w.dirty[0] = 1;
    UNSCOPED_INFO("ACFF8: one bit is a header, three fillers at most and 16 words");
    REQUIRE(LowererFlushWords(w) == 20);
    w.dirty[0] = 0b111;
    UNSCOPED_INFO("ACFF8: a run of three bits shares one header");
    REQUIRE(LowererFlushWords(w) == 52);
    w.dirty[0] = 0b101;
    UNSCOPED_INFO("ACFF8: two runs");
    REQUIRE(LowererFlushWords(w) == 40);
    w.dirty[0] = 0;
    w.dirty[1] = 1;
    UNSCOPED_INFO("ACFF8 on G+0x08");
    REQUIRE(LowererFlushWords(w) == 20);
    w.dirty[1] = 0;
    w.dirty[2] = 0b11;
    UNSCOPED_INFO("ACC60 on G+0x10 bits 0..11");
    REQUIRE(LowererFlushWords(w) == 3);
    w.dirty[2] = 1ULL << 58; // outside every group
    UNSCOPED_INFO("G+0x10 bit 58 is no group's");
    REQUIRE(LowererFlushWords(w) == 0);
    w.dirty[2] = 1ULL << 21;
    UNSCOPED_INFO("G+0x10 bit 21");
    REQUIRE(LowererFlushWords(w) == 2);
    w.dirty[2] = 0;
    w.dirty[3] = 1;
    UNSCOPED_INFO("ACEA0: a header word or two, six words per bit, four closing words");
    REQUIRE(LowererFlushWords(w) == 2 + 6 + 4);
    w.dirty[3] = 1ULL << 34;
    UNSCOPED_INFO("G+0x18 bit 34 through ACC60");
    REQUIRE(LowererFlushWords(w) == 2);
    w.dirty[3] = 0;
    w.dirty[4] = 1ULL << 49;
    UNSCOPED_INFO("ACD20: 13 words, a header word or two, four words per bit");
    REQUIRE(LowererFlushWords(w) == 13 + 2 + 4);
    w.dirty[4] = 1ULL << 56;
    UNSCOPED_INFO("G+0x20 bit 56: a 40-bit run");
    REQUIRE(LowererFlushWords(w) == 41);
    w.dirty[4] = 1ULL << 48;
    UNSCOPED_INFO("G+0x20 bit 48 is no group's");
    REQUIRE(LowererFlushWords(w) == 0);
    w.dirty[4] = 0;

    // 0x826AC8C8 when the mask meets G+0x10.
    w.dirty[2]    = 1ULL << 22;
    w.mask        = 1ULL << 22;
    w.targetCount = 3;
    UNSCOPED_INFO("AC8C8 for three targets");
    REQUIRE(LowererFlushWords(w) == 32 + 5 * 3 + 2);
    w.targetCount = 17;
    UNSCOPED_INFO("AC8C8 past the loop bound");
    REQUIRE(LowererFlushWords(w) == UINT64_MAX);
    w.mask        = 0;
    w.targetCount = 3;
    UNSCOPED_INFO("the mask does not meet the word");
    REQUIRE(LowererFlushWords(w) == 2);

    // 0x826ADD88 words follow guest memory: unbounded.
    w.dirty[2] = 0x20000;
    UNSCOPED_INFO("G+0x10 bit 17 is 0x826ADD88's");
    REQUIRE(LowererFlushWords(w) == UINT64_MAX);

    // The bound holds every emulated sequence at every cursor alignment, and is reached at one.
    const std::uint64_t patterns[5][5] = {
        { 1, 0, 0, 0, 0 },
        { 0b10110, 0b1, 0, 0, 0 },
        { 0, 0, 0xFFF | (0x1FULL << 12) | (1ULL << 30) | (0xFFFFULL << 42), 0x00000000F0F0F0F0ULL | (0x1FFFFFULL << 34), 0x3FFFFFFFFFULL },
        { ~0ULL, ~0ULL, 0, 0, 0 },
        { 0, 0, 0, 0xFFFFFFFFULL, (0x3FULL << 49) | (1ULL << 56) | (0xFFULL << 38) },
    };
    for (const auto& pattern : patterns)
    {
        Device d{};
        for (std::size_t i = 0; i < 5; ++i)
            d.dirty[i] = pattern[i] & ~(i == 2 ? std::uint64_t{ 0x1E0000 } : 0ULL);
        const auto    e     = entryOf();
        LowererWatch  watch = watchOf(d, e);
        const auto    bound = LowererFlushWords(watch);
        std::uint64_t most  = 0;
        for (const std::uint32_t low : { 0U, 4U, 8U, 12U })
        {
            std::uint32_t end   = 0;
            const auto    words = emulateFlush(d, kRing + 0x100 + low, end);
            UNSCOPED_INFO(std::format("bound {} below emulated {} at cursor nibble {}", bound, words, low));
            REQUIRE(words <= bound);
            UNSCOPED_INFO("the emulated cursor advanced by its words");
            REQUIRE(end == kRing + 0x100 + low + 4 * words);
            most = std::max<std::uint64_t>(most, words);
        }
        UNSCOPED_INFO(std::format("bound {} is far above the worst emulated {}", bound, most));
        REQUIRE(most + 3 >= bound);
    }
}

void expectState(Device d, const LowererEntry& e, const std::string& name)
{
    // The recorded stride byte follows the call's, unless the vertex shader is clear for the test to show it does not matter.
    if (d.vertexShader)
        d.strideWord = (e.stride >> 2) << 24;
    const auto         watch   = watchOf(d, e);
    LowererExitRefusal refusal = LowererExitRefusal::None;
    std::string        why;
    const auto         exit = DeriveLowererExitState(e, watch, &refusal, &why);
    const auto         emu  = emulate(d, e);
    UNSCOPED_INFO(name + ": the emulation must reach the success epilogue");
    REQUIRE(emu.ok);
    UNSCOPED_INFO(name + ": the model refused: " + why);
    REQUIRE(exit.has_value());
    UNSCOPED_INFO(name + ": refusal reported");
    REQUIRE(refusal == LowererExitRefusal::None);
    UNSCOPED_INFO(name + ": limits");
    REQUIRE((exit->limit34 == emu.limit34 && exit->limit38 == emu.limit38));
    UNSCOPED_INFO(name + ": pool words");
    REQUIRE((exit->poolPos == emu.poolPos && exit->wrapPos == emu.wrapPos && exit->poolUsed == emu.poolUsed));
    UNSCOPED_INFO(name + ": buffers");
    REQUIRE((exit->vertexDest == emu.vertexDest && exit->indexDest == emu.indexDest));
    UNSCOPED_INFO(name + ": dword counts");
    REQUIRE((exit->vertexDwords == emu.vertexDwords && exit->indexDwords == emu.indexDwords));
    UNSCOPED_INFO(name + ": the staged end is the entry cursor");
    REQUIRE(exit->pendingEnd == d.cursor);
    UNSCOPED_INFO(name + ": dirty words");
    REQUIRE((exit->dirty[3] == (d.dirty[3] | 1) && exit->dirty[0] == d.dirty[0] && exit->dirty[2] == d.dirty[2]));
    UNSCOPED_INFO(name + ": flush bound");
    REQUIRE(exit->flushWords >= emu.flushWords);
    UNSCOPED_INFO(name + ": ring words");
    REQUIRE(exit->pm4Words == (d.tilingByte ? kLowererPm4TiledWords : kLowererPm4Words));
    UNSCOPED_INFO(name + ": shape");
    REQUIRE(exit->tiled == (d.tilingByte != 0));
}

void expectRefusal(const Device& d, const LowererEntry& e, LowererExitRefusal expected, const std::string& name)
{
    const auto         watch   = watchOf(d, e);
    LowererExitRefusal refusal = LowererExitRefusal::None;
    std::string        why;
    const auto         exit = DeriveLowererExitState(e, watch, &refusal, &why);
    UNSCOPED_INFO(name + ": the model must refuse");
    REQUIRE(!exit.has_value());
    UNSCOPED_INFO(std::format("{}: refusal {} instead of {} ({})", name, kLowererExitRefusalNames[static_cast<std::size_t>(refusal)], kLowererExitRefusalNames[static_cast<std::size_t>(expected)], why));
    REQUIRE(refusal == expected);
    UNSCOPED_INFO(name + ": a reason");
    REQUIRE(!why.empty());
}

void testFixedPool()
{
    Device d{};
    expectState(d, entryOf(), "fixed pool, fresh");
    // The takes see each other: the position moves, and the index take is below the vertex take.
    {
        const auto watch = watchOf(d, entryOf());
        const auto exit  = DeriveLowererExitState(entryOf(), watch);
        UNSCOPED_INFO("descending 16-byte buffers");
        REQUIRE((exit && exit->indexDest < exit->vertexDest && exit->vertexDest % 16 == 0 && exit->indexDest % 16 == 0));
        UNSCOPED_INFO("the position is the last buffer");
        REQUIRE(exit->poolPos == exit->indexDest);
        UNSCOPED_INFO("G+0x3A54 gains both byte counts");
        REQUIRE(exit->poolUsed == 3200 + 604);
        UNSCOPED_INFO("limits move by the removed bytes");
        REQUIRE((exit->limit34 == d.limit34 - ((kPoolBase + kPoolSize - exit->indexDest) & ~3U) && exit->limit38 == d.limit38 - ((kPoolBase + kPoolSize - exit->indexDest) & ~3U)));
    }
    d.poolPos  = kPoolBase + 0x2000 + 4;
    d.poolUsed = 1000;
    expectState(d, entryOf(), "fixed pool, unaligned position");
    d.poolPos  = 0;
    d.dirty[0] = 0b11;
    d.dirty[3] = 0x8;
    d.dirty[4] = 1ULL << 38;
    d.cursor   = kRing + 0x104;
    expectState(d, entryOf(), "dirty groups below the limits");
    d.dirty[2] = 1ULL << 21;
    expectState(d, entryOf(3, 0, 4, 12), "empty indices, 32-bit flag");
    expectState(d, entryOf(0, 5, 0, 32), "no vertices");
    expectState(d, entryOf(1, 1, 0, 4), "one vertex of four bytes");
    // A tiled root has 27 ring words and the same allocation.
    Device tiled{};
    tiled.tilingByte = 0x21;
    expectState(tiled, entryOf(), "tiled");
    // Odd pool size: the aligned pool end is the top.
    Device odd{};
    odd.poolSize = kPoolSize + 3;
    expectState(odd, entryOf(), "pool size not a multiple of four");
}

void testNullPool()
{
    Device d{};
    d.poolObject = 0;
    d.sideEnd    = 0x40800000;
    d.wrapLimit  = 0x40900000;
    d.limit34    = kRing + 0x40000;
    d.segment    = 2;
    d.progress   = 2;
    expectState(d, entryOf(), "null pool, fresh");
    {
        const auto watch = watchOf(d, entryOf());
        const auto exit  = DeriveLowererExitState(entryOf(), watch);
        UNSCOPED_INFO("the null pool moves neither limit");
        REQUIRE((exit && exit->limit34 == d.limit34 && exit->limit38 == d.limit38));
        UNSCOPED_INFO("the first buffer is the aligned start");
        REQUIRE(exit->vertexDest == ((d.limit34 + 60 + 15) & ~15U));
        UNSCOPED_INFO("the second follows the first");
        REQUIRE(exit->indexDest == ((exit->vertexDest + 3200 + 15) & ~15U));
        UNSCOPED_INFO("the position is the end of the second take");
        REQUIRE(exit->poolPos == exit->indexDest + 604);
    }
    d.poolPos = d.sideEnd - 0x2000 + 8;
    expectState(d, entryOf(), "null pool, position inside the block");
    // A wrap: the first take overruns the side end, the second follows it.
    Device wrap   = d;
    wrap.poolPos  = wrap.sideEnd - 100;
    wrap.wrapPos  = wrap.sideEnd + 0x1000;
    wrap.segment  = 2;
    wrap.progress = 3; // one segment behind the wrapped take, its end inside the block
    wrap.progress = 3;
    {
        expectState(wrap, entryOf(), "null pool wrap");
        const auto watch = watchOf(wrap, entryOf());
        const auto exit  = DeriveLowererExitState(entryOf(), watch);
        UNSCOPED_INFO("a wrap moves the wrap position");
        REQUIRE(exit->wrapPos != wrap.wrapPos);
    }
}

void testRefusals()
{
    const auto e = entryOf();
    Device     base{};

    {
        Device d     = base;
        d.tilingByte = 0x01;
        expectRefusal(d, e, LowererExitRefusal::Tiling, "tiling byte 0x01");
        d.tilingByte = 0x20;
        expectRefusal(d, e, LowererExitRefusal::Tiling, "tiling byte 0x20");
    }
    {
        Device d   = base;
        d.dirty[2] = 0x20000;
        expectRefusal(d, e, LowererExitRefusal::StateDirty, "shader dirty bit");
        d.dirty[2] = 0x100000;
        expectRefusal(d, e, LowererExitRefusal::StateDirty, "last shader dirty bit");
        d.dirty[2] = 0x1000000 | 0x10000;
        expectState(d, e, "bits next to the shader mask");
    }
    {
        // A stride byte that differs from the recorded one, with a vertex shader (g63:13084-13106).
        const auto changed = entryOf(100, 301, 0, 36);
        expectRefusal(base, changed, LowererExitRefusal::StateDirty, "stride change");
        Device d       = base;
        d.vertexShader = 0;
        expectState(d, changed, "stride change without a vertex shader");
    }
    {
        Device d    = base;
        d.allocByte = 0x20;
        expectRefusal(d, e, LowererExitRefusal::AllocMarker, "alloc marker");
        d.allocByte = 0xDF;
        expectState(d, e, "other alloc bits");
    }
    {
        Device d     = base;
        d.tilingByte = 0x21;
        d.tileCursor = d.tileLimit - 15;
        expectRefusal(d, e, LowererExitRefusal::TileGrowth, "tile list full");
        d.tileCursor = d.tileLimit - 16;
        expectState(d, e, "tile list with room for exactly one record");
    }
    {
        Device d   = base;
        d.poolBase = 0;
        expectRefusal(d, e, LowererExitRefusal::AllocPath, "pool object without a buffer");
    }
    {
        // The ring: the flush words against the soft and hard limits.
        Device d   = base;
        d.dirty[0] = 1;
        d.cursor   = kRing + 0x100;
        d.limit38  = d.cursor + 4 * 20 - 4;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "flush words past the soft limit");
        d.limit38 = d.cursor + 4 * 20;
        expectState(d, e, "flush words exactly at the soft limit");
        d.limit34 = d.cursor + 4 * 20;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "flush words reaching the hard limit");
        d             = base;
        d.dirty[2]    = 1ULL << 22;
        d.mask        = d.dirty[2];
        d.targetCount = 99;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "unbounded flush");
        d         = base;
        d.cursor  = 0xFFFFFFA0;
        d.limit34 = 0xFFFFFFFF;
        d.limit38 = 0xFFFFFFF0;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "ring end near the top of memory");
    }
    {
        // A take past the pool threshold with the bracket clear moves the limits (g93:12837-12865).
        Device d    = base;
        d.threshold = 3200 + 604 - 1;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "threshold with the bracket clear");
        d.bracket = 1;
        expectState(d, e, "threshold with the bracket set");
        d           = base;
        d.threshold = 3200 - 1;
        d.poolUsed  = 0;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "the first take alone passes the threshold");
        d = base;
        // Limits that the removed bytes would take below zero.
        d.cursor  = 0x10;
        d.limit38 = 0x20;
        d.limit34 = 0x30;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "limits below the removed bytes");
    }
    {
        // A fixed pool whose room is short of the cursor plus 164.
        Device d   = base;
        d.poolSize = 3200 + 604 - 1;
        d.poolBase = d.cursor + 164 - 3200 + 8; // the top lies just above the cursor plus the takes
        expectRefusal(d, e, LowererExitRefusal::AllocCapacity, "fixed pool too small");
    }
    {
        Device d     = base;
        d.poolObject = 0;
        d.sideEnd    = 0x40800000;
        d.wrapLimit  = 0x40900000;
        d.segment    = 2;
        d.progress   = 2;
        d.poolPos    = d.sideEnd - 8;
        d.wrapPos    = d.wrapLimit - 8;
        expectRefusal(d, e, LowererExitRefusal::AllocCapacity, "null pool past the wrap limit");
        d.wrapLimit = 0x40900000 + 0x100000;
        d.wrapPos   = d.sideEnd + 0x100;
        d.progress  = 0; // the GPU is two segments behind the wrapped take
        expectRefusal(d, e, LowererExitRefusal::AllocWait, "null pool wait");
        d.progress = 3;
        expectState(d, e, "null pool wrap without a wait");
    }
    {
        // The first null-pool take passes the threshold; the sum of both wraps back under it.
        Device d     = base;
        d.poolObject = 0;
        d.sideEnd    = 0x40800000;
        d.wrapLimit  = 0x40900000;
        d.limit34    = kRing + 0x40000;
        d.segment    = 2;
        d.progress   = 2;
        d.poolUsed   = 0U - 3200 - 300;
        d.threshold  = 0xFFFFFE00;
        expectRefusal(d, e, LowererExitRefusal::RingSpace, "null pool first take past the threshold, sum wrapping");
    }
}

void testWrites()
{
    Device d{};
    d.dirty[3]       = 0x10;
    const auto e     = entryOf();
    const auto watch = watchOf(d, e);
    const auto exit  = DeriveLowererExitState(e, watch);
    UNSCOPED_INFO("state");
    REQUIRE(exit.has_value());
    const auto writes = LowererExitWrites(e, watch, *exit);
    // Every word that changes, in order: G+0x1C, G+0x34, G+0x38, G+0x3A44, G+0x3A54, G+0x3484..0x3494 (G+0x3A4C is unchanged), the out words.
    std::map<std::uint32_t, std::uint32_t> byAddress;
    for (const auto& write : writes)
        byAddress[write.address] = write.value;
    UNSCOPED_INFO("the dirty low word gains bit 0");
    REQUIRE(byAddress.at(kG + 0x1C) == 0x11);
    UNSCOPED_INFO("limits");
    REQUIRE((byAddress.at(kG + 0x34) == exit->limit34 && byAddress.at(kG + 0x38) == exit->limit38));
    UNSCOPED_INFO("pool words");
    REQUIRE((byAddress.at(kG + 0x3A44) == exit->poolPos && byAddress.at(kG + 0x3A54) == exit->poolUsed));
    UNSCOPED_INFO("an unchanged word is not stored");
    REQUIRE(!byAddress.count(kG + 0x3A4C));
    UNSCOPED_INFO("the staged end");
    REQUIRE(byAddress.at(kG + 0x3484) == d.cursor);
    UNSCOPED_INFO("staged buffers");
    REQUIRE((byAddress.at(kG + 0x3488) == exit->vertexDest && byAddress.at(kG + 0x348C) == exit->indexDest));
    UNSCOPED_INFO("staged dword counts");
    REQUIRE((byAddress.at(kG + 0x3490) == 800 && byAddress.at(kG + 0x3494) == 151));
    UNSCOPED_INFO("out words");
    REQUIRE((byAddress.at(kVertexOut) == exit->vertexDest && byAddress.at(kIndexOut) == exit->indexDest));
    UNSCOPED_INFO("no address is stored twice");
    REQUIRE(writes.size() == byAddress.size());
    UNSCOPED_INFO("the out words are stored last");
    REQUIRE((writes.back().address == kIndexOut && writes[writes.size() - 2].address == kVertexOut));

    // A word that already holds the value is not stored, but the out words always are.
    Device same{};
    same.dirty[3]        = 1;
    const auto sameWatch = watchOf(same, e);
    auto       sameExit  = *DeriveLowererExitState(e, sameWatch);
    UNSCOPED_INFO("bit 0 already set");
    REQUIRE(LowererExitWrites(e, sameWatch, sameExit).front().address != kG + 0x1C);
    Device staged{};
    staged.staging[0]      = staged.cursor;
    const auto stagedWatch = watchOf(staged, e);
    const auto stagedExit  = *DeriveLowererExitState(e, stagedWatch);
    for (const auto& write : LowererExitWrites(e, stagedWatch, stagedExit))
    {
        UNSCOPED_INFO("the staged end already equals the cursor");
        REQUIRE(write.address != kG + 0x3484);
    }

    // The writes applied to a memory give the exit watch.
    Memory memory = build(d, e);
    for (const auto& write : writes)
        memory[write.address] = write.value;
    LowererWatch after;
    UNSCOPED_INFO("exit unreadable");
    REQUIRE(ReadLowererWatch(readerOf(memory), e, after));
    UNSCOPED_INFO("the stores give the exit words");
    REQUIRE((after.vertexOut == exit->vertexDest && after.indexOut == exit->indexDest && after.pendingEnd == d.cursor && (after.dirty[3] & 0xFFFFFFFF) == 0x11));
    UNSCOPED_INFO("the stores give the allocator words");
    REQUIRE((after.limit34 == exit->limit34 && after.poolPos == exit->poolPos && after.poolUsed == exit->poolUsed && after.vertexDwords == 800 && after.indexDwords == 151));
}

#if REREVVED_DEVELOPER
// The exit watch of an Original that ran: the model's state with the flush groups' zeroing and the ring words.
LowererWatch originalExit(const LowererWatch& entry, const LowererExitState& state, std::uint32_t flushWords)
{
    LowererWatch out = entry;
    out.dirty        = { 0, 0, 0, 1, 0 };
    out.cursor       = entry.cursor + 4 * flushWords;
    out.pendingEnd   = out.cursor + 4 * state.pm4Words;
    out.limit34      = state.limit34;
    out.limit38      = state.limit38;
    out.poolPos      = state.poolPos;
    out.poolUsed     = state.poolUsed;
    out.wrapPos      = state.wrapPos;
    out.vertexDest   = state.vertexDest;
    out.indexDest    = state.indexDest;
    out.vertexDwords = state.vertexDwords;
    out.indexDwords  = state.indexDwords;
    out.vertexOut    = state.vertexDest;
    out.indexOut     = state.indexDest;
    if (state.tiled)
        out.tileCursor = entry.tileCursor + kLowererTileRecordBytes;
    return out;
}

void testMismatches()
{
    Device d{};
    d.dirty[0]       = 1;
    const auto e     = entryOf();
    const auto watch = watchOf(d, e);
    const auto exit  = *DeriveLowererExitState(e, watch);
    const auto after = originalExit(watch, exit, 12);
    UNSCOPED_INFO("an exact Original has no mismatch");
    REQUIRE(LowererExitMismatches(exit, watch, after, 0) == 0);
    UNSCOPED_INFO("no flush words");
    REQUIRE(LowererExitMismatches(exit, watch, originalExit(watch, exit, 0), 0) == 0);
    UNSCOPED_INFO("flush words at the bound");
    REQUIRE(LowererExitMismatches(exit, watch, originalExit(watch, exit, static_cast<std::uint32_t>(exit.flushWords)), 0) == 0);

    const auto flag = [&](std::size_t word)
    {
        return 1U << word;
    };
    const auto mutate = [&](const char* name, std::size_t word, auto&& change)
    {
        auto x = after;
        change(x);
        const auto mask = LowererExitMismatches(exit, watch, x, 0);
        UNSCOPED_INFO(std::format("{}: mask {:X} instead of bit {} ({})", name, mask, word, kLowererExitWordNames[word]));
        REQUIRE(mask == flag(word));
    };
    UNSCOPED_INFO("result");
    REQUIRE(LowererExitMismatches(exit, watch, after, 5) == flag(0));
    mutate("dirty_00", 1, [](LowererWatch& x)
           {
               x.dirty[0] = 1;
           });
    mutate("dirty_08", 2, [](LowererWatch& x)
           {
               x.dirty[1] = 1;
           });
    mutate("dirty_10", 3, [](LowererWatch& x)
           {
               x.dirty[2] = 4;
           });
    mutate("dirty_18", 4, [](LowererWatch& x)
           {
               x.dirty[3] = 3;
           });
    mutate("dirty_20", 5, [](LowererWatch& x)
           {
               x.dirty[4] = 2;
           });
    mutate("limit_34", 6, [](LowererWatch& x)
           {
               x.limit34 += 4;
           });
    mutate("limit_38", 7, [](LowererWatch& x)
           {
               x.limit38 += 4;
           });
    mutate("pool_pos", 8, [](LowererWatch& x)
           {
               x.poolPos += 16;
           });
    mutate("pool_used", 9, [](LowererWatch& x)
           {
               x.poolUsed += 4;
           });
    mutate("wrap_pos", 10, [](LowererWatch& x)
           {
               x.wrapPos += 16;
           });
    mutate("vertex_dest", 11, [](LowererWatch& x)
           {
               x.vertexDest += 16;
           });
    mutate("index_dest", 12, [](LowererWatch& x)
           {
               x.indexDest += 16;
           });
    mutate("vertex_dwords", 13, [](LowererWatch& x)
           {
               x.vertexDwords += 1;
           });
    mutate("index_dwords", 14, [](LowererWatch& x)
           {
               x.indexDwords += 1;
           });
    mutate("vertex_out", 15, [](LowererWatch& x)
           {
               x.vertexOut += 16;
           });
    mutate("index_out", 16, [](LowererWatch& x)
           {
               x.indexOut += 16;
           });
    mutate("alloc_byte", 17, [](LowererWatch& x)
           {
               x.allocByte |= 0x20;
           });
    mutate("pm4_words", 18, [](LowererWatch& x)
           {
               x.pendingEnd += 4;
           });
    mutate("ring_words", 19, [&](LowererWatch& x)
           {
               x.cursor += 4 * 1000;
               x.pendingEnd = x.cursor + 4 * exit.pm4Words;
           });
    mutate("tile_record", 20, [](LowererWatch& x)
           {
               x.tileCursor += 16;
           });
    mutate("fence", 21, [](LowererWatch& x)
           {
               x.fence += 1;
           });
    // A cursor that moved backwards, or by a partial word, is a ring mismatch too.
    {
        auto x       = after;
        x.cursor     = watch.cursor - 4;
        x.pendingEnd = x.cursor + 4 * exit.pm4Words;
        UNSCOPED_INFO("a cursor that moved back");
        REQUIRE(LowererExitMismatches(exit, watch, x, 0) == flag(19));
        x            = after;
        x.cursor     = watch.cursor + 6;
        x.pendingEnd = x.cursor + 4 * exit.pm4Words;
        UNSCOPED_INFO("a cursor off a word");
        REQUIRE(LowererExitMismatches(exit, watch, x, 0) == flag(19));
    }
    // A tiled model expects the list cursor 16 on, and an untiled one expects it where it was.
    Device t{};
    t.tilingByte     = 0x21;
    const auto tw    = watchOf(t, e);
    const auto texit = *DeriveLowererExitState(e, tw);
    UNSCOPED_INFO("tiled Original");
    REQUIRE(LowererExitMismatches(texit, tw, originalExit(tw, texit, 0), 0) == 0);
    auto untiledAfter       = originalExit(tw, texit, 0);
    untiledAfter.tileCursor = tw.tileCursor;
    UNSCOPED_INFO("a tiled model needs the record");
    REQUIRE(LowererExitMismatches(texit, tw, untiledAfter, 0) == flag(20));

    UNSCOPED_INFO("observed flush words");
    REQUIRE(LowererObservedFlushWords(watch, after) == 12);
    auto back   = after;
    back.cursor = watch.cursor - 4;
    UNSCOPED_INFO("a cursor that moved back counts nothing");
    REQUIRE(LowererObservedFlushWords(watch, back) == 0);
    back.cursor = watch.cursor + 6;
    UNSCOPED_INFO("a partial word counts nothing");
    REQUIRE(LowererObservedFlushWords(watch, back) == 0);
}

void testCounters()
{
    (void)TakeLowererExitCounts();
    NoteLowererExit(0, false, 12);
    NoteLowererExit(0, true, 40);
    NoteLowererExit((1U << 8) | (1U << 19), true, 3);
    NoteLowererExitUnmodeled(LowererExitRefusal::RingSpace);
    NoteLowererExitUnmodeled(LowererExitRefusal::RingSpace);
    NoteLowererExitUnmodeled(LowererExitRefusal::StateDirty);
    NoteLowererExitUnreadable();
    const auto counts = TakeLowererExitCounts();
    UNSCOPED_INFO("tallies");
    REQUIRE((counts.ok == 2 && counts.tiled == 2 && counts.tiledOk == 1 && counts.unreadable == 1));
    UNSCOPED_INFO("mismatch words");
    REQUIRE((counts.mismatch[8] == 1 && counts.mismatch[19] == 1 && counts.mismatch[0] == 0));
    UNSCOPED_INFO("flush words");
    REQUIRE((counts.flushWordsMax == 40 && counts.flushWordsSum == 55 && counts.flushWordsCalls == 3));
    UNSCOPED_INFO("unmodeled");
    REQUIRE((counts.unmodeled[static_cast<std::size_t>(LowererExitRefusal::RingSpace)] == 2 && counts.unmodeled[static_cast<std::size_t>(LowererExitRefusal::StateDirty)] == 1));
    const auto line = FormatLowererExitCounts(counts);
    UNSCOPED_INFO("ok fields: " + line);
    REQUIRE((line.find(" lowerer_exit_ok=2 ") != std::string::npos && line.find(" lowerer_exit_tiled_ok=1 ") != std::string::npos));
    UNSCOPED_INFO("mismatch fields: " + line);
    REQUIRE((line.find(" lowerer_exit_mismatch_pool_pos=1") != std::string::npos && line.find(" lowerer_exit_mismatch_ring_words=1") != std::string::npos));
    UNSCOPED_INFO("every mismatch field is printed: " + line);
    REQUIRE(line.find(" lowerer_exit_mismatch_result=0") != std::string::npos);
    UNSCOPED_INFO("unmodeled fields: " + line);
    REQUIRE((line.find(" lowerer_exit_unmodeled_ring_space=2") != std::string::npos && line.find(" lowerer_exit_unmodeled_state_dirty=1") != std::string::npos));
    UNSCOPED_INFO("a zero unmodeled count is not printed: " + line);
    REQUIRE(line.find("lowerer_exit_unmodeled_tiling") == std::string::npos);
    const auto empty = TakeLowererExitCounts();
    UNSCOPED_INFO("a take resets the counts");
    REQUIRE((empty.ok == 0 && empty.flushWordsCalls == 0));
    UNSCOPED_INFO("word names");
    REQUIRE(kLowererExitWordNames.size() == kLowererExitWordCount);
}
#endif

} // namespace

TEST_CASE("lowerer_derive", "[ctest][lowerer-derive][timeout-60]")
{
    testDraw();
    testRead();
    testFlushWords();
    testFixedPool();
    testNullPool();
    testRefusals();
    testWrites();
#if REREVVED_DEVELOPER
    testMismatches();
    testCounters();
#endif
}
