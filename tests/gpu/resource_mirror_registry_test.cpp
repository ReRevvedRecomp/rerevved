#include "gpu/guest_d3d/resource_mirror_registry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace rerevved::gpu::guest_d3d;

void put(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset]     = std::uint8_t(value >> 24);
    bytes[offset + 1] = std::uint8_t(value >> 16);
    bytes[offset + 2] = std::uint8_t(value >> 8);
    bytes[offset + 3] = std::uint8_t(value);
}

// Guest memory whose every byte derives from its address and a write epoch.
struct Memory
{
    std::uint8_t epoch = 1;
    int          reads = 0;

    std::vector<std::uint8_t> bytes(std::uint32_t address, std::uint32_t size) const
    {
        std::vector<std::uint8_t> result(size);
        for (std::uint32_t i = 0; i < size; ++i)
            result[i] = std::uint8_t((address + i) * 7 + epoch * 31);
        return result;
    }

    GuestResourceMirrors::RangeRead reader()
    {
        return [this](std::uint32_t address, std::uint32_t size)
        {
            ++reads;
            return GuestBytes(bytes(address, size));
        };
    }
};

GuestResourceMirrors::Config watched()
{
    return { .watch = [](std::uint32_t, std::uint32_t)
             {
                 return true;
             } };
}

// The loading logo fetch: tiled k_8_8_8_8 1024x256, one level at 1BBBA000.
constexpr std::array<std::uint32_t, 6> kLogo{ 0x88004802, 0x1BBBA086, 0x001FE3FF, 0x00A80C14, 0x003F0003, 0x00000218 };
constexpr std::uint32_t                kLogoBytes = 1024 * 256 * 4;

constexpr std::uint32_t kVertexHeader = 0x40001000, kIndexHeader = 0x40002000, kTextureHeader = 0x40003000, kSurfaceHeader = 0x40004000;

GuestResourceRegistration vertexBuffer(std::uint32_t header = kVertexHeader, std::uint32_t address = 0x0A000000, std::uint32_t size = 256)
{
    GuestResourceRegistration registration;
    registration.kind      = GuestResourceKind::VertexBuffer;
    registration.header    = header;
    registration.words     = { address | 3, size << 2 };
    registration.ranges[0] = { address, size };
    return registration;
}

GuestResourceRegistration logoTexture()
{
    GuestResourceRegistration registration;
    registration.kind      = GuestResourceKind::Texture;
    registration.header    = kTextureHeader;
    registration.words     = kLogo;
    registration.ranges[0] = { 0x1BBBA000, kLogoBytes };
    return registration;
}

GuestResourceRead stream(std::uint32_t address = 0x0A000000, std::uint32_t size = 256)
{
    return { GuestResourceKind::VertexBuffer, address, size, 0, { address | 3, size << 2 } };
}

void describe()
{
    // A texture header holds the fetch words at +0x1C; this base is a CPU
    // alias at E0000000 and above, which SetTexture moves up a page.
    std::array<std::uint8_t, kGuestTextureHeaderBytes> texture{};
    for (std::size_t i = 0; i < kLogo.size(); ++i)
        put(texture, 0x1C + i * 4, kLogo[i]);
    put(texture, 0x20, 0xFBBB9086);
    GuestResourceRegistration registration;
    UNSCOPED_INFO("a texture header describes");
    CHECK(DescribeGuestResource(GuestResourceKind::Texture, kTextureHeader, texture, registration));
    UNSCOPED_INFO("the E0000000 alias adds 0x1000 as SetTexture does");
    CHECK(registration.words == kLogo);
    UNSCOPED_INFO("the texture range is its base level");
    CHECK((registration.ranges[0].address == 0x1BBBA000 && registration.ranges[0].size == kLogoBytes));
    UNSCOPED_INFO("a short texture header is refused");
    CHECK(!DescribeGuestResource(GuestResourceKind::Texture, kTextureHeader, std::span<const std::uint8_t>(texture).first(0x30), registration));

    std::array<std::uint8_t, kGuestBufferHeaderBytes> buffer{};
    put(buffer, 0x18, 0x8A000003);
    put(buffer, 0x1C, 64 << 2);
    UNSCOPED_INFO("a vertex buffer header describes");
    CHECK(DescribeGuestResource(GuestResourceKind::VertexBuffer, kVertexHeader, buffer, registration));
    UNSCOPED_INFO("a vertex buffer binds its physical fetch pair over 64 dwords");
    CHECK((registration.words[0] == 0x0A000003 && registration.words[1] == 256 && registration.ranges[0].address == 0x0A000000 && registration.ranges[0].size == 256));

    put(buffer, 0x00, 0x00000002);
    put(buffer, 0x18, 0x8B000000);
    put(buffer, 0x1C, 128);
    UNSCOPED_INFO("an index buffer header describes");
    CHECK(DescribeGuestResource(GuestResourceKind::IndexBuffer, kIndexHeader, buffer, registration));
    UNSCOPED_INFO("an index buffer keeps its object words and its physical range");
    CHECK((registration.words[0] == 0x8B000000 && registration.words[1] == 128 && registration.words[2] == 2 && registration.ranges[0].address == 0x0B000000 &&
           registration.ranges[0].size == 128));

    std::array<std::uint8_t, kGuestSurfaceHeaderBytes> surface{};
    UNSCOPED_INFO("a surface is not registered");
    CHECK(!DescribeGuestResource(GuestResourceKind::Surface, kSurfaceHeader, surface, registration));
}

void uploadThenHit()
{
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    const auto           serial = mirrors.Register(vertexBuffer());
    UNSCOPED_INFO("registration assigns a serial");
    CHECK((serial && mirrors.SerialOf(kVertexHeader) == serial && mirrors.Objects() == 1));
    const auto first = mirrors.Bind(stream(), memory.reader());
    UNSCOPED_INFO("the first bind uploads the whole range");
    CHECK((first && *first == memory.bytes(0x0A000000, 256) && memory.reads == 1));
    const auto second = mirrors.Bind(stream(), memory.reader());
    UNSCOPED_INFO("a clean bind shares the buffer without a read");
    CHECK((second && first && second->SharesBuffer(*first) && memory.reads == 1));
    UNSCOPED_INFO("the registry holds the captured range");
    CHECK(mirrors.HeldBytes() == 256);

    // SetStreamSource with an offset moves the base and size by the same bytes.
    const auto offset = mirrors.Bind(stream(0x0A000040, 192), memory.reader());
    UNSCOPED_INFO("an offset stream is a sub-view");
    CHECK((offset && *offset == memory.bytes(0x0A000040, 192) && memory.reads == 1));
    const auto again = mirrors.Bind(stream(0x0A000040, 192), memory.reader());
    UNSCOPED_INFO("a repeated sub-view shares one copy");
    CHECK((again && offset && again->SharesBuffer(*offset)));

    auto counts = mirrors.TakeCounts();
    UNSCOPED_INFO("counts one upload and four hits");
    CHECK((counts.hits == 4 && counts.uploads == 1 && counts.uploadBytes == 256 && counts.fallbacks == 0));
    counts = mirrors.TakeCounts();
    UNSCOPED_INFO("TakeCounts resets");
    CHECK((!counts.hits && !counts.uploads));
}

void recheck()
{
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    mirrors.Register(vertexBuffer());
    UNSCOPED_INFO("a stream that ends before its buffer falls back");
    CHECK(!mirrors.Bind(stream(0x0A000000, 128), memory.reader()));
    auto untyped = stream();
    untyped.words[0] &= ~3U;
    UNSCOPED_INFO("a fetch without the vertex type bits falls back");
    CHECK(!mirrors.Bind(untyped, memory.reader()));
    UNSCOPED_INFO("a read beyond the range falls back");
    CHECK(!mirrors.Bind(stream(0x0A000100, 16), memory.reader()));
    UNSCOPED_INFO("an unregistered range falls back");
    CHECK(!mirrors.Bind(stream(0x0C000000, 16), memory.reader()));
    UNSCOPED_INFO("fallbacks are counted and read nothing");
    CHECK((mirrors.TakeCounts().fallbacks == 4 && memory.reads == 0));

    mirrors.Register(logoTexture());
    GuestResourceRead read{ GuestResourceKind::Texture, 0x1BBBA000, kLogoBytes, 0, kLogo };
    const auto        bound = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("a texture binds by its fetch");
    CHECK((bound && memory.reads == 1));
    read.words[0] ^= 1U << 10; // clamp X
    read.words[5] ^= 0x1FFU;   // the mip word's sampler bits
    const auto sampled = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("sampler and clamp bits do not recheck");
    CHECK((sampled && bound && sampled->SharesBuffer(*bound)));
    read.words[0] ^= 1U << 22; // pitch
    UNSCOPED_INFO("another pitch falls back");
    CHECK(!mirrors.Bind(read, memory.reader()));
    const GuestResourceRead asStream{ GuestResourceKind::VertexBuffer, 0x1BBBA000, kLogoBytes, 0, { 0x1BBBA003, kLogoBytes << 2 } };
    UNSCOPED_INFO("a texture's storage does not bind as a stream");
    CHECK(!mirrors.Bind(asStream, memory.reader()));
}

// Hints reuse identical live reads; registration/release/changed reads retry.
void hints()
{
    Memory                memory;
    GuestResourceMirrors  mirrors(watched());
    const auto            first = mirrors.Register(vertexBuffer());
    GuestResourceBindHint hint;
    auto                  read = stream();
    read.hint                  = &hint;
    const auto plain           = mirrors.Bind(stream(), memory.reader());
    const auto hinted          = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("a hinted bind finds the object a plain bind does");
    CHECK((plain && hinted && hinted->SharesBuffer(*plain) && hint.found && hint.serial == first));
    const auto again = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("a reused hint binds the same copy");
    CHECK((again && again->SharesBuffer(*plain) && memory.reads == 1));

    // A newer object over the storage is a newer match.
    const auto second = mirrors.Register(vertexBuffer(kVertexHeader + 0x100));
    const auto newer  = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("a registration sends a hint back to the lookup");
    CHECK((newer && !newer->SharesBuffer(*plain) && hint.serial == second && memory.reads == 2));
    mirrors.Released(kVertexHeader + 0x100);
    const auto older = mirrors.Bind(read, memory.reader());
    UNSCOPED_INFO("a released object's hint finds the live one");
    CHECK((older && older->SharesBuffer(*plain) && hint.serial == first));

    // Another read through the same hint is looked up for itself.
    auto untyped = read;
    untyped.words[0] &= ~3U;
    UNSCOPED_INFO("a hint made for other words is not reused");
    CHECK((!mirrors.Bind(untyped, memory.reader()) && !hint.found));
    UNSCOPED_INFO("the hint is rewritten for the read it serves");
    CHECK((mirrors.Bind(read, memory.reader()) && hint.found && hint.serial == first));

    // A lookup that found nothing holds only until a registration.
    GuestResourceBindHint missing;
    auto                  elsewhere = stream(0x0C000000, 64);
    elsewhere.hint                  = &missing;
    UNSCOPED_INFO("an unregistered range falls back through its hint");
    CHECK((!mirrors.Bind(elsewhere, memory.reader()) && !missing.found));
    mirrors.Register(vertexBuffer(kVertexHeader + 0x200, 0x0C000000, 64));
    UNSCOPED_INFO("a registration lets an unfound hint bind");
    CHECK((mirrors.Bind(elsewhere, memory.reader()) && missing.found));

    // A replacement registry in the same generation, with an unrelated
    // object under the hint's serial, looks the read up for itself.
    GuestResourceBindHint carried;
    auto                  carriedRead = stream();
    carriedRead.hint                  = &carried;
    {
        GuestResourceMirrors replaced(watched());
        replaced.Register(vertexBuffer());
        UNSCOPED_INFO("the first registry's hint names its object");
        CHECK((replaced.Bind(carriedRead, memory.reader()) && carried.found && carried.serial == 1));
    }
    GuestResourceMirrors replacement(watched());
    replacement.Register(vertexBuffer(kVertexHeader, 0x0B000000, 64));
    const auto reads = memory.reads;
    UNSCOPED_INFO("another registry's hint is not reused");
    CHECK((!replacement.Bind(carriedRead, memory.reader()) && !carried.found && memory.reads == reads));
}

void dirtyRanges()
{
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    mirrors.Register(vertexBuffer());
    const auto first = mirrors.Bind(stream(), memory.reader());
    ++memory.epoch;
    mirrors.Written(0x0A0000F0, 0x0A000100);
    const auto written = mirrors.Bind(stream(), memory.reader());
    UNSCOPED_INFO("a write notice rereads the range");
    CHECK((written && *written == memory.bytes(0x0A000000, 256) && memory.reads == 2));
    UNSCOPED_INFO("a held bind keeps its bytes");
    CHECK((first && *first == Memory{}.bytes(0x0A000000, 256)));
    mirrors.Written(0x0A000100, 0x0A000200);
    mirrors.Written(0x09000000, 0x0A000000);
    UNSCOPED_INFO("writes beside the range leave it clean");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 2));

    ++memory.epoch;
    mirrors.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    UNSCOPED_INFO("a locked object is not captured");
    CHECK((!mirrors.Bind(stream(), memory.reader()) && memory.reads == 2));
    mirrors.Unlocked(kVertexHeader);
    const auto unlocked = mirrors.Bind(stream(), memory.reader());
    UNSCOPED_INFO("an unlock rereads the range");
    CHECK((unlocked && *unlocked == memory.bytes(0x0A000000, 256) && memory.reads == 3));
    mirrors.TakeCounts();
    const auto notice = GuestResourceMirrors::WriteNotice(&mirrors, 0x0A000000, 4, false);
    UNSCOPED_INFO("the write notice unwatches only its pages");
    CHECK((notice.first == 0x0A000000 && notice.second == 4));
    GuestResourceMirrors::WriteNotice(&mirrors, 0x0A000000, 4, false);
    UNSCOPED_INFO("a notice over a captured range counts once");
    CHECK(mirrors.TakeCounts().watchDirty == 1);
    UNSCOPED_INFO("the SDK write notice dirties the range");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 4));
    mirrors.Written(0x0A000000, 0x0A000004);
    UNSCOPED_INFO("host writes are not watch writes");
    CHECK(mirrors.TakeCounts().watchDirty == 0);
    mirrors.Unlocked(kVertexHeader);
    UNSCOPED_INFO("an unlock without a lock still dirties");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 5));

    // A write while the bind reads: the bind returns its bytes and keeps none.
    ++memory.epoch;
    mirrors.Written(0x0A000000, 0x0A000001);
    const GuestResourceMirrors::RangeRead racing = [&](std::uint32_t address, std::uint32_t size)
    {
        auto bytes = memory.reader()(address, size);
        mirrors.Written(address, std::uint64_t(address) + size);
        return bytes;
    };
    mirrors.TakeCounts();
    const auto raced = mirrors.Bind(stream(0x0A000040, 192), racing);
    UNSCOPED_INFO("a raced bind returns the bytes it read");
    CHECK((raced && *raced == memory.bytes(0x0A000040, 192) && !raced->IsShared()));
    auto counts = mirrors.TakeCounts();
    UNSCOPED_INFO("a raced bind is a fallback");
    CHECK((counts.fallbacks == 1 && counts.uploads == 0));
    UNSCOPED_INFO("the next bind uploads again");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 7 && mirrors.TakeCounts().uploads == 1));

    // Resolve destinations stay on the bytes path.
    mirrors.Written(0x0A000000, 0x0A000100, true);
    UNSCOPED_INFO("a resolved range is not mirrored");
    CHECK(!mirrors.Bind(stream(), memory.reader()));
    mirrors.Written(0x0A000000, 0x0A000100);
    UNSCOPED_INFO("a resolved range stays resolved");
    CHECK((!mirrors.Bind(stream(), memory.reader()) && memory.reads == 7));
    counts = mirrors.TakeCounts();
    UNSCOPED_INFO("resolved reads are counted apart from fallbacks");
    CHECK((counts.resolved == 2 && counts.fallbacks == 0));
}

void watchAndBudget()
{
    Memory               memory;
    GuestResourceMirrors unwatched({});
    unwatched.Register(vertexBuffer());
    UNSCOPED_INFO("an unset watch falls back");
    CHECK((!unwatched.Bind(stream(), memory.reader()) && memory.reads == 0));
    GuestResourceMirrors refused({ .watch = [](std::uint32_t, std::uint32_t)
                                   {
                                       return false;
                                   } });
    refused.Register(vertexBuffer());
    UNSCOPED_INFO("a range the watch cannot cover falls back");
    CHECK((!refused.Bind(stream(), memory.reader()) && refused.TakeCounts().fallbacks == 1 && memory.reads == 0));

    auto config   = watched();
    config.budget = 100;
    GuestResourceMirrors small(config);
    small.Register(vertexBuffer());
    const auto over = small.Bind(stream(0x0A000040, 192), memory.reader());
    UNSCOPED_INFO("a range over the budget returns its read and keeps nothing");
    CHECK((over && *over == memory.bytes(0x0A000040, 192) && small.HeldBytes() == 0));
    const auto counts = small.TakeCounts();
    UNSCOPED_INFO("a capture over the budget is a fallback");
    CHECK((counts.fallbacks == 1 && counts.uploads == 0));

    GuestResourceMirrors truncated(watched());
    truncated.Register(vertexBuffer());
    const GuestResourceMirrors::RangeRead shortRead = [](std::uint32_t, std::uint32_t)
    {
        return GuestBytes(std::vector<std::uint8_t>(8));
    };
    UNSCOPED_INFO("a short read falls back");
    CHECK(!truncated.Bind(stream(), shortRead));
}

void retire()
{
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    const auto           first = mirrors.Register(vertexBuffer());
    const auto           held  = mirrors.Bind(stream(), memory.reader());
    mirrors.Released(kVertexHeader);
    UNSCOPED_INFO("a released object stops binding at once");
    CHECK((mirrors.Objects() == 0 && mirrors.Entries() == 1 && mirrors.SerialOf(kVertexHeader) == first));
    UNSCOPED_INFO("a released object falls back and drops its buffer");
    CHECK((!mirrors.Bind(stream(), memory.reader()) && mirrors.HeldBytes() == 0));
    UNSCOPED_INFO("the next bind erases the released entry");
    CHECK((mirrors.Entries() == 0 && !mirrors.SerialOf(kVertexHeader)));
    UNSCOPED_INFO("a frame's held bytes outlive the release");
    CHECK((held && *held == Memory{}.bytes(0x0A000000, 256)));

    // A new object over the released storage binds.
    const auto second = mirrors.Register(vertexBuffer(kVertexHeader + 0x100));
    UNSCOPED_INFO("a new object takes the released storage");
    CHECK((second > first && mirrors.Objects() == 1));
    UNSCOPED_INFO("the replacement binds");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 2));
    const auto third = mirrors.Register(vertexBuffer(kVertexHeader + 0x100, 0x0A000000, 128));
    UNSCOPED_INFO("a re-registered header replaces its object");
    CHECK((third > second && mirrors.SerialOf(kVertexHeader + 0x100) == third && mirrors.Objects() == 1 && mirrors.HeldBytes() == 0));

    // Live overlapping objects both stay until the device goes.
    mirrors.Register(vertexBuffer(kVertexHeader + 0x200, 0x0A000000, 128));
    UNSCOPED_INFO("live overlapping objects both stay");
    CHECK(mirrors.Objects() == 2);
    UNSCOPED_INFO("an overlapped range binds");
    CHECK(mirrors.Bind(stream(0x0A000000, 128), memory.reader()).has_value());
    mirrors.ReleasedAll();
    UNSCOPED_INFO("a device release releases and erases every object");
    CHECK((mirrors.Objects() == 0 && !mirrors.Bind(stream(0x0A000000, 128), memory.reader()) && mirrors.HeldBytes() == 0 && mirrors.Entries() == 0));
    mirrors.Released(0x12345678);
    mirrors.Locked(GuestResourceKind::Texture, 0x12345678);
    mirrors.Unlocked(0x12345678);

    // A free with its heap extent releases every object intersecting the
    // allocation, wherever it starts.
    GuestResourceMirrors sized(watched());
    sized.Register(vertexBuffer(kVertexHeader, 0x0D000000, 0x100));
    sized.Register(vertexBuffer(kVertexHeader + 0x100, 0x0D008000, 0x100));
    sized.Register(vertexBuffer(kVertexHeader + 0x200, 0x0CFFFF00, 0x200));
    sized.Register(vertexBuffer(kVertexHeader + 0x300, 0x0D010000, 0x100));
    sized.Register(vertexBuffer(kVertexHeader + 0x400, 0x0CFFFE00, 0x100));
    sized.Freed(0x0D000000, 0x10000);
    UNSCOPED_INFO("a sized free releases every object intersecting its allocation");
    CHECK((sized.Objects() == 2 && !sized.Bind(stream(0x0D008000, 0x100), memory.reader()) && !sized.Bind(stream(0x0D000000, 0x100), memory.reader()) &&
           sized.Bind(stream(0x0D010000, 0x100), memory.reader()) && sized.Bind(stream(0x0CFFFE00, 0x100), memory.reader())));
    UNSCOPED_INFO("a sized free is not counted as unsized");
    CHECK(sized.TakeCounts().freeUnsized == 0);

    // Without an extent, a freed backing releases the objects over it, by address.
    GuestResourceMirrors freed(watched());
    freed.Register(vertexBuffer(kVertexHeader, 0x0A000000, 256));
    freed.Register(vertexBuffer(kVertexHeader + 0x100, 0x0A000800, 256));
    freed.Register(vertexBuffer(kVertexHeader + 0x200, 0x0A001000, 256));
    freed.Register(vertexBuffer(kVertexHeader + 0x300, 0x09FFFF00, 0x200));
    freed.Freed(0x0A000000);
    UNSCOPED_INFO("an unsized free releases objects starting in its page or holding its address");
    CHECK((freed.Objects() == 1 && freed.Bind(stream(0x0A001000, 256), memory.reader()) && !freed.Bind(stream(0x0A000800, 256), memory.reader())));
    UNSCOPED_INFO("an unsized free is counted");
    CHECK(freed.TakeCounts().freeUnsized == 1);
    UNSCOPED_INFO("released entries are erased at the next bind");
    CHECK(freed.Entries() == 1);
    freed.Register(vertexBuffer(kVertexHeader + 0x400, 0x0A000000, 256));
    UNSCOPED_INFO("a reused backing binds its new object");
    CHECK((freed.Objects() == 2 && freed.Bind(stream(), memory.reader())));

    // The free names no size: the span runs to the end of the largest range
    // starting in the freed page, and every object starting in it goes.
    GuestResourceMirrors allocation(watched());
    allocation.Register(vertexBuffer(kVertexHeader, 0x0B000000, 0x10000));
    allocation.Register(vertexBuffer(kVertexHeader + 0x100, 0x0B008000, 0x100));
    allocation.Register(vertexBuffer(kVertexHeader + 0x200, 0x0B00FF00, 0x100));
    allocation.Register(vertexBuffer(kVertexHeader + 0x300, 0x0B010000, 0x100));
    allocation.Freed(0x0B000000);
    UNSCOPED_INFO("a free releases objects inside the largest range at its base");
    CHECK((allocation.Objects() == 1 && allocation.Bind(stream(0x0B010000, 0x100), memory.reader()) && !allocation.Bind(stream(0x0B008000, 0x100), memory.reader())));
}

void pendingLocks()
{
    // A lock before the first registration carries over to it.
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    mirrors.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    mirrors.Register(vertexBuffer(), true);
    UNSCOPED_INFO("a lock opened before registration keeps the object uncaptured");
    CHECK((!mirrors.Bind(stream(), memory.reader()) && memory.reads == 0));
    mirrors.Unlocked(kVertexHeader);
    UNSCOPED_INFO("its unlock lets the next bind capture");
    CHECK((mirrors.Bind(stream(), memory.reader()) && memory.reads == 1));

    // A lock closed before registration leaves nothing open.
    mirrors.Locked(GuestResourceKind::VertexBuffer, kVertexHeader + 0x100);
    mirrors.Unlocked(kVertexHeader + 0x100);
    mirrors.Register(vertexBuffer(kVertexHeader + 0x100, 0x0C000000, 256));
    UNSCOPED_INFO("a closed pending lock leaves the object bindable");
    CHECK((mirrors.Bind(stream(0x0C000000, 256), memory.reader()) && memory.reads == 2));

    // A lock after a release goes to the pending set, and its unlock closes
    // it there: the re-registered object binds.
    GuestResourceMirrors released(watched());
    Memory               fresh;
    released.Register(vertexBuffer());
    released.Released(kVertexHeader);
    released.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    released.Unlocked(kVertexHeader);
    released.Register(vertexBuffer());
    released.TakeCounts();
    UNSCOPED_INFO("an unlock after a release closes the pending lock");
    CHECK((released.Bind(stream(), fresh.reader()) && released.Bind(stream(), fresh.reader()) && fresh.reads == 1));
    UNSCOPED_INFO("the re-registered object binds without fallback");
    CHECK(released.TakeCounts().fallbacks == 0);

    // A lock open on an object when it is released moves to the pending set,
    // and its unlock closes it there.
    released.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    released.Released(kVertexHeader);
    released.Unlocked(kVertexHeader);
    released.Register(vertexBuffer());
    released.TakeCounts();
    UNSCOPED_INFO("a released object's closed lock leaves its successor bindable");
    CHECK((released.Bind(stream(), fresh.reader()) && fresh.reads == 2 && released.TakeCounts().fallbacks == 0));

    // A lock still open at a release carries to the successor: the
    // predecessor's late unlock closes it, not the successor's own lock.
    GuestResourceMirrors late(watched());
    Memory               lateMemory;
    late.Register(vertexBuffer());
    late.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    late.Released(kVertexHeader);
    late.Register(vertexBuffer());
    late.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    late.Unlocked(kVertexHeader);
    late.TakeCounts();
    UNSCOPED_INFO("a successor stays locked while the guest holds the other lock");
    CHECK((!late.Bind(stream(), lateMemory.reader()) && lateMemory.reads == 0 && late.TakeCounts().fallbacks == 1));
    late.Unlocked(kVertexHeader);
    UNSCOPED_INFO("the last unlock lets the successor capture and hit");
    CHECK((late.Bind(stream(), lateMemory.reader()) && late.Bind(stream(), lateMemory.reader()) && lateMemory.reads == 1 && late.TakeCounts().fallbacks == 0));

    // A free over a released object's storage ends the locks it still held:
    // an unrelated object reusing its header binds.
    GuestResourceMirrors reused(watched());
    Memory               reusedMemory;
    reused.Register(vertexBuffer());
    reused.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    reused.Released(kVertexHeader);
    reused.Locked(GuestResourceKind::VertexBuffer, kVertexHeader + 0x100);
    reused.Freed(0x0A000000, 0x1000);
    reused.Register(vertexBuffer(kVertexHeader, 0x0E000000, 256));
    reused.Register(vertexBuffer(kVertexHeader + 0x100, 0x0E001000, 256));
    reused.TakeCounts();
    UNSCOPED_INFO("a free ends the locks its released object held");
    CHECK((reused.Bind(stream(0x0E000000, 256), reusedMemory.reader()) && reused.TakeCounts().fallbacks == 0));
    UNSCOPED_INFO("a free keeps locks opened before any registration");
    CHECK(!reused.Bind(stream(0x0E001000, 256), reusedMemory.reader()));

    // An unlock of a pending header closes the inherited lock first, so a
    // free leaves the newer lock open on the successor.
    GuestResourceMirrors mixed(watched());
    Memory               mixedMemory;
    mixed.Register(vertexBuffer());
    mixed.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    mixed.Released(kVertexHeader);
    mixed.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    mixed.Unlocked(kVertexHeader);
    mixed.Freed(0x0A000000, 0x1000);
    mixed.Register(vertexBuffer());
    UNSCOPED_INFO("a free keeps a pending lock not known to be inherited");
    CHECK((!mixed.Bind(stream(), mixedMemory.reader()) && mixedMemory.reads == 0));
    mixed.Unlocked(kVertexHeader);
    UNSCOPED_INFO("the last unlock lets the successor capture");
    CHECK((mixed.Bind(stream(), mixedMemory.reader()) && mixedMemory.reads == 1));

    // A re-registration during an open lock keeps it open.
    mirrors.Locked(GuestResourceKind::VertexBuffer, kVertexHeader);
    mirrors.Register(vertexBuffer(), true);
    UNSCOPED_INFO("a replaced object's open lock carries over");
    CHECK(!mirrors.Bind(stream(), memory.reader()));
    mirrors.Unlocked(kVertexHeader);
    UNSCOPED_INFO("and closes at its unlock");
    CHECK(mirrors.Bind(stream(), memory.reader()).has_value());
}

void views()
{
    // Sub-range copies count against the held total; an object keeps eight.
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    mirrors.Register(vertexBuffer(kVertexHeader, 0x0A000000, 4096));
    std::size_t expected = 4096;
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        const auto bound = mirrors.Bind(stream(0x0A000000 + 16 * i, 4096 - 16 * i), memory.reader());
        UNSCOPED_INFO("an offset stream binds its bytes");
        CHECK((bound && *bound == memory.bytes(0x0A000000 + 16 * i, 4096 - 16 * i)));
        if (i >= 2)
            expected += 4096 - 16 * i;
    }
    UNSCOPED_INFO("the eight newest views are held with the range");
    CHECK((memory.reads == 1 && mirrors.HeldBytes() == expected));
    auto counts = mirrors.TakeCounts();
    UNSCOPED_INFO("the oldest view is evicted past the cap");
    CHECK((counts.viewEvictions == 1 && counts.hits == 10));
    const auto again = mirrors.Bind(stream(0x0A000000 + 16 * 9, 4096 - 16 * 9), memory.reader());
    const auto same  = mirrors.Bind(stream(0x0A000000 + 16 * 9, 4096 - 16 * 9), memory.reader());
    UNSCOPED_INFO("a held view is shared");
    CHECK((again && same && again->SharesBuffer(*same)));

    // A view the budget cannot hold is served from the copy without keeping it.
    auto config   = watched();
    config.budget = 4096 + 100;
    GuestResourceMirrors small(config);
    small.Register(vertexBuffer(kVertexHeader, 0x0A000000, 4096));
    const auto offset = small.Bind(stream(0x0A000010, 4096 - 16), memory.reader());
    UNSCOPED_INFO("a view over the budget is not kept");
    CHECK((offset && *offset == memory.bytes(0x0A000010, 4096 - 16) && small.HeldBytes() == 4096));
    counts = small.TakeCounts();
    UNSCOPED_INFO("an unkept view is counted as an eviction");
    CHECK((counts.viewEvictions == 1 && counts.hits == 1 && counts.fallbacks == 0));
}

void bindsAndLocks()
{
    Memory                    memory;
    GuestResourceMirrors      mirrors(watched());
    GuestResourceRegistration index;
    index.kind      = GuestResourceKind::IndexBuffer;
    index.header    = kIndexHeader;
    index.words     = { 0x8B000000, 128, 2 };
    index.ranges[0] = { 0x0B000000, 128 };
    mirrors.Register(index);
    GuestResourceRead draw{ GuestResourceKind::IndexBuffer, 0x0B000010, 32, kIndexHeader, { 0x8B000000, 128 } };
    const auto        indices = mirrors.Bind(draw, memory.reader());
    UNSCOPED_INFO("an index range binds by its object");
    CHECK((indices && *indices == memory.bytes(0x0B000010, 32)));
    draw.words[1] = 64;
    UNSCOPED_INFO("an object whose words changed falls back");
    CHECK(!mirrors.Bind(draw, memory.reader()));
    draw.words[1] = 128;
    draw.header   = kIndexHeader + 4;
    UNSCOPED_INFO("an unregistered object falls back");
    CHECK(!mirrors.Bind(draw, memory.reader()));

    // A bind hook registers a header only when it does not hold its words.
    UNSCOPED_INFO("the registry holds a registered object");
    CHECK(mirrors.Holds(GuestResourceKind::IndexBuffer, kIndexHeader, { 0x8B000000, 128, 2 }));
    UNSCOPED_INFO("other words or another kind are not held");
    CHECK((!mirrors.Holds(GuestResourceKind::IndexBuffer, kIndexHeader, { 0x8B000000, 64, 2 }) &&
           !mirrors.Holds(GuestResourceKind::VertexBuffer, kIndexHeader, { 0x8B000000, 128, 2 })));
    auto sampled = kLogo;
    sampled[0] ^= 1U << 10;
    mirrors.Register(logoTexture(), true);
    UNSCOPED_INFO("a texture is held by its storage words");
    CHECK((mirrors.Holds(GuestResourceKind::Texture, kTextureHeader, sampled) && mirrors.TakeCounts().bindRegistrations == 1));

    // A level lock names its texture; locks are counted by the locked kind.
    const GuestResourceRead read{ GuestResourceKind::Texture, 0x1BBBA000, kLogoBytes, 0, kLogo };
    mirrors.Bind(read, memory.reader());
    const auto reads = memory.reads;
    mirrors.Locked(GuestResourceKind::Surface, kTextureHeader);
    UNSCOPED_INFO("a texture with an open level lock falls back");
    CHECK(!mirrors.Bind(read, memory.reader()));
    mirrors.Unlocked(kTextureHeader);
    UNSCOPED_INFO("the unlock core dirties the texture");
    CHECK((mirrors.Bind(read, memory.reader()) && memory.reads == reads + 1));
    const GuestResourceRead indexRead{ GuestResourceKind::IndexBuffer, 0x0B000010, 32, kIndexHeader, { 0x8B000000, 128 } };
    mirrors.Locked(GuestResourceKind::IndexBuffer, kIndexHeader);
    mirrors.Locked(GuestResourceKind::IndexBuffer, kIndexHeader);
    mirrors.Unlocked(kIndexHeader);
    UNSCOPED_INFO("nested locks stay open");
    CHECK(!mirrors.Bind(indexRead, memory.reader()));
    mirrors.Unlocked(kIndexHeader);
    UNSCOPED_INFO("the last unlock closes them");
    CHECK(mirrors.Bind(indexRead, memory.reader()).has_value());
    const auto counts = mirrors.TakeCounts();
    UNSCOPED_INFO("locks are counted by kind");
    CHECK((counts.locks[std::size_t(GuestResourceKind::Surface)] == 1 && counts.locks[std::size_t(GuestResourceKind::IndexBuffer)] == 2 &&
           counts.locks[std::size_t(GuestResourceKind::Texture)] == 0));

    std::array<std::uint8_t, kGuestBufferHeaderBytes> buffer{};
    put(buffer, 0x18, 0x8A000003);
    put(buffer, 0x1C, 0x10000002 | 256);
    std::array<std::uint32_t, 6> words{};
    UNSCOPED_INFO("a vertex buffer header gives its fetch pair");
    CHECK((GuestResourceWords(GuestResourceKind::VertexBuffer, buffer, words) && words[0] == 0x0A000003 && words[1] == (0x10000002U | 256)));
    GuestResourceRegistration registration;
    UNSCOPED_INFO("the size word flag bits are not length");
    CHECK((DescribeGuestResource(GuestResourceKind::VertexBuffer, kVertexHeader, buffer, registration) && registration.ranges[0].size == 256));
}

// Newest live covering object wins; a locked intended object falls back.
void manyObjects()
{
    constexpr std::uint32_t kCount = 600, kBase = 0x0A000000, kStride = 0x2000, kBytes = 0x400, kHeaderBase = 0x50000000;
    const auto              header = [](std::uint32_t i)
    {
        return kHeaderBase + i * 0x40;
    };
    const auto address = [](std::uint32_t i)
    {
        return kBase + i * kStride;
    };
    Memory               memory;
    GuestResourceMirrors mirrors(watched());
    for (std::uint32_t i = 0; i < kCount; ++i)
        mirrors.Register(vertexBuffer(header(i), address(i), kBytes));
    // One large range, so a lookup has many objects to rule out below it.
    mirrors.Register(vertexBuffer(0x5FFF0000, kBase, kCount * kStride));

    for (int round = 0; round < 2; ++round)
        for (std::uint32_t i = 0; i < kCount; i += 7)
        {
            const auto bound = mirrors.Bind(stream(address(i), kBytes), memory.reader());
            UNSCOPED_INFO("a bind among many objects serves its own range");
            CHECK((bound && *bound == memory.bytes(address(i), kBytes)));
        }
    auto       counts = mirrors.TakeCounts();
    const auto binds  = (kCount + 6) / 7;
    UNSCOPED_INFO("repeated binds share the first capture");
    CHECK((counts.hits == 2 * binds && counts.uploads == binds && counts.fallbacks == 0));

    // A newer object over the same range wins; locking it shows the choice.
    constexpr std::uint32_t kTarget = 301, kNewer = 0x50010000, kNewest = 0x50010040;
    mirrors.Register(vertexBuffer(kNewer, address(kTarget), kBytes));
    mirrors.Locked(GuestResourceKind::VertexBuffer, kNewer);
    UNSCOPED_INFO("a newer registration is chosen over the cached older object");
    CHECK(!mirrors.Bind(stream(address(kTarget), kBytes), memory.reader()));
    mirrors.Locked(GuestResourceKind::VertexBuffer, header(kTarget));
    mirrors.Unlocked(kNewer);
    UNSCOPED_INFO("an unlocked newer object serves the bind while the older one is locked");
    CHECK((mirrors.Bind(stream(address(kTarget), kBytes), memory.reader()) && mirrors.TakeCounts().fallbacks == 1));
    mirrors.Unlocked(header(kTarget));

    // Releasing the newer object returns the bind to the older one.
    mirrors.Released(kNewer);
    mirrors.Locked(GuestResourceKind::VertexBuffer, header(kTarget));
    UNSCOPED_INFO("a released object stops binding at once and the older object answers");
    CHECK(!mirrors.Bind(stream(address(kTarget), kBytes), memory.reader()));
    mirrors.Unlocked(header(kTarget));

    // Three registrations over one range: the newest is chosen, and once it is
    // released the oldest live one answers again.
    mirrors.Register(vertexBuffer(kNewest, address(kTarget), kBytes));
    mirrors.Locked(GuestResourceKind::VertexBuffer, kNewest);
    UNSCOPED_INFO("the newest of several registrations over a range is chosen");
    CHECK(!mirrors.Bind(stream(address(kTarget), kBytes), memory.reader()));
    mirrors.Released(kNewest);
    mirrors.Unlocked(kNewest);
    UNSCOPED_INFO("the older live object answers once the newest is released");
    CHECK(bool(mirrors.Bind(stream(address(kTarget), kBytes), memory.reader())));

    // The large range is a stream of its own.
    const auto whole = mirrors.Bind(stream(kBase, kCount * kStride), memory.reader());
    UNSCOPED_INFO("the large range binds as itself");
    CHECK((whole && whole->size() == kCount * kStride));

    // A free over the target's storage releases it and the large range with
    // it; cached answers for both stop binding.
    mirrors.Freed(address(kTarget), kBytes);
    UNSCOPED_INFO("a freed object no longer binds from a cached answer");
    CHECK(!mirrors.Bind(stream(address(kTarget), kBytes), memory.reader()));
    UNSCOPED_INFO("a freed large range no longer binds from a cached answer");
    CHECK(!mirrors.Bind(stream(kBase, kCount * kStride), memory.reader()));
    mirrors.TakeCounts();

    // Index buffers go by their object header among many.
    for (std::uint32_t i = 0; i < kCount; ++i)
    {
        GuestResourceRegistration index;
        index.kind      = GuestResourceKind::IndexBuffer;
        index.header    = 0x60000000 + i * 0x40;
        index.words     = { 0x8C000000 + i * 0x100, 0x80, 0 };
        index.ranges[0] = { GuestPhysicalAddress(index.words[0]), 0x80 };
        mirrors.Register(index);
    }
    for (std::uint32_t i = 0; i < kCount; i += 11)
    {
        const auto physical = GuestPhysicalAddress(0x8C000000 + i * 0x100);
        const auto bound    = mirrors.Bind({ GuestResourceKind::IndexBuffer, physical, 0x80, 0x60000000 + i * 0x40, { 0x8C000000 + i * 0x100, 0x80 } }, memory.reader());
        UNSCOPED_INFO("an index buffer binds by its header");
        CHECK((bound && *bound == memory.bytes(physical, 0x80)));
        UNSCOPED_INFO("another object's header with these words falls back");
        CHECK(!mirrors.Bind({ GuestResourceKind::IndexBuffer, physical, 0x80, 0x60000000 + ((i + 1) % kCount) * 0x40, { 0x8C000000 + i * 0x100, 0x80 } }, memory.reader()));
    }
}

void formatAndPublish()
{
    GuestResourceMirrorCounts counts;
    counts.hits                                                = 12;
    counts.fallbacks                                           = 3;
    counts.uploads                                             = 2;
    counts.uploadBytes                                         = 1500000;
    counts.resolved                                            = 1;
    counts.watchDirty                                          = 4;
    counts.viewEvictions                                       = 6;
    counts.bindRegistrations                                   = 5;
    counts.freeUnsized                                         = 7;
    counts.locks[std::size_t(GuestResourceKind::VertexBuffer)] = 2;
    const auto fields                                          = FormatGuestResourceMirrorCounts(counts);
    UNSCOPED_INFO("the timing fields:" + fields);
    CHECK(fields == " mirror_hits=12 mirror_fallbacks=3 mirror_uploads=2 mirror_upload_mb=1.5 mirror_resolved=1 mirror_watch_dirty=4 mirror_bind_registrations=5 "
                    "mirror_view_evictions=6 mirror_free_unsized=7 mirror_locks_vertex_buffer=2");

    UNSCOPED_INFO("nothing is published by default");
    CHECK((!PublishedGuestResourceMirrors() && !guestResourceMirrorsEnabled.load()));
    auto mirrors = std::make_shared<GuestResourceMirrors>(watched());
    PublishGuestResourceMirrors(mirrors);
    UNSCOPED_INFO("a published registry enables the hooks");
    CHECK((guestResourceMirrorsEnabled.load() && PublishedGuestResourceMirrors() == mirrors));
    mirrors.reset();
    UNSCOPED_INFO("a destroyed registry disables the hooks");
    CHECK((!PublishedGuestResourceMirrors() && !guestResourceMirrorsEnabled.load()));
}

} // namespace

TEST_CASE("resource_mirror_registry", "[ctest][resource-mirror-registry]")
{
    describe();
    uploadThenHit();
    recheck();
    hints();
    dirtyRanges();
    watchAndBudget();
    retire();
    pendingLocks();
    views();
    bindsAndLocks();
    manyObjects();
    formatAndPublish();
}
