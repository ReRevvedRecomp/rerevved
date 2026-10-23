#include "gpu/diagnostics/route_effect_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "manual_cases.hpp"
#include "production_observers.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <rex/crypto/sha256.h>
#include <rex/hook.h>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <toml++/toml.hpp>
#include <tuple>

using namespace rerevved::gpu::diagnostics;

namespace
{

std::array<std::uint8_t, 0x3500> guestState{};
std::array<std::uint8_t, 64>     guestZero{};

std::span<const std::uint8_t> guestMemory(std::uint64_t address, std::uint64_t size, bool)
{
    if (address == 0x2000 && size == guestState.size())
        return guestState;
    if (address == 0x3000 || address == 0x4000 || address == 0x5000 || address == 0x1054)
        return std::span(guestZero).first(size);
    throw std::runtime_error("scene_test_shader_read");
}

template <class Predicate>
void await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate())
    {
        UNSCOPED_INFO("recorder test timed out");
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::array<std::uint8_t, 0x10000> writerGuest{};
std::array<std::uint8_t, 0x4000>  writerPhysical{};
bool                              failWriterDestination = false, failWriterRelease = false;

std::span<const std::uint8_t> writerMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if ((failWriterDestination && address >= 0xA0000000ULL) || (failWriterRelease && address == 0x2000))
        throw std::runtime_error("scene_writer_fixture_unreadable");
    if (physical && address + size <= writerPhysical.size())
        return std::span(writerPhysical).subspan(address, size);
    if (address >= 0xA0000000ULL && address + size <= 0xA0000000ULL + writerPhysical.size())
        return std::span(writerPhysical).subspan(address - 0xA0000000ULL, size);
    if (address + size <= writerGuest.size())
        return std::span(writerGuest).subspan(address, size);
    throw std::runtime_error("scene_writer_fixture_extent");
}

void writerWord(std::size_t address, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        writerGuest[address + i] = std::uint8_t(value >> (24 - 8 * i));
}

int writerTest()
{
    SetSceneTestMemoryReader(writerMemory);
    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-writers-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start writer fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error));
    auto*      opaque = sceneCaptureSession.load();
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    ctx.lr     = 0x82521AD0;
    // SDK fetch layout: linear RGBA8 32x32, pitch 32, physical base 0x1000.
    writerWord(0x201C, (1U << 22) | 2);
    writerWord(0x2020, 0xA0001006);
    writerWord(0x2024, 31 | (31 << 13));
    writerWord(0x2030, 1U << 9);
    const auto creation = BeginSceneEntry(opaque, 0x82533DB8, ctx);
    ctx.r3.u32          = 0x2000;
    EndSceneEntry(creation, ctx);
    writerGuest[0x8000] = 'a';
    ctx.r3.u32          = 0xA0001000;
    ctx.r4.u32          = 0x8000;
    ctx.r5.u32          = 1;
    const auto copy     = BeginSceneEntry(opaque, 0x827F8550, ctx);
    UNSCOPED_INFO("inside texture copy is admitted before arm");
    REQUIRE(copy.session != nullptr);
    writerPhysical[0x1000] = 'b';
    EndSceneEntry(copy, ctx);
    ctx.r3.u32 = 0xA0003000;
    UNSCOPED_INFO("outside texture copy is filtered");
    REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, ctx).session);
    ctx.r3.u32          = 0xA0000FFF;
    ctx.r5.u32          = 2;
    const auto straddle = BeginSceneEntry(opaque, 0x827F8550, ctx);
    UNSCOPED_INFO("straddling copy is admitted");
    REQUIRE(straddle.session != nullptr);
    EndSceneEntry(straddle, ctx);

    // sub_826B5F30 entry args and stack +54/+5C/+64; two RGBA8 rows.
    ctx         = {};
    ctx.r1.u32  = 0x1000;
    ctx.lr      = 0x82521AD0;
    ctx.r3.u32  = 0xA0001000;
    ctx.r4.u32  = 16;
    ctx.r5.u32  = 2;
    ctx.r6.u32  = 2;
    ctx.r7.u32  = 6;
    ctx.r9.u32  = 0x8000;
    ctx.r10.u32 = 8;
    writerWord(0x1054, 6);
    writerWord(0x1064, 0x1234);
    const auto a = BeginSceneEntry(opaque, 0x826B5F30, ctx);
    ctx.r3.u32   = 0;
    EndSceneEntry(a, ctx);

    // sub_82521558 .199.cpp: pixel +50/+54/+5C/+60/+64; pair pitch/address.
    writerWord(0x3050, 0x8000);
    writerWord(0x3054, 0x5000);
    writerWord(0x305C, 0x5100);
    writerWord(0x3060, 1);
    writerWord(0x3064, 1);
    writerWord(0x5000, 8);
    writerWord(0x5104, 16);
    writerWord(0x5200, 8);
    writerWord(0x5204, 0xA0001000);
    writerWord(0x5304, 2);
    writerWord(0x5308, 2);
    writerWord(0x5310, 6);
    ctx          = {};
    ctx.r1.u32   = 0x1000;
    ctx.lr       = 0x82521F74;
    ctx.r3.u32   = 0x3000;
    ctx.r5.u32   = 0x5200;
    ctx.r6.u32   = 0x5300;
    ctx.r7.u32   = 0x5300;
    const auto b = BeginSceneEntry(opaque, 0x82521558, ctx);
    EndSceneEntry(b, ctx);

    // sub_82305648 .10.cpp: owner +28/+40, dst XY then source LTRB.
    writerWord(0x601C, 0x6100);
    writerWord(0x6028, 0x2000);
    for (unsigned i = 0; i < 6; ++i)
        writerWord(0x6200 + i * 4, i + 1);
    writerWord(0x6300, 9);
    writerWord(0x630C, 4);
    writerWord(0x6310, 0x8000);
    writerWord(0x6314, 16);
    ctx               = {};
    ctx.r1.u32        = 0x1000;
    ctx.lr            = 0x82300000;
    ctx.r3.u32        = 0x6000;
    ctx.r4.u32        = 2;
    ctx.r5.u32        = 1;
    ctx.r6.u32        = 0x6200;
    ctx.r7.u32        = 0x6300;
    const auto c      = BeginSceneEntry(opaque, 0x82305648, ctx);
    auto       nested = ctx;
    nested.r3.u32     = 0xA0001000;
    nested.r4.u32     = 0x8000;
    nested.r5.u32     = 4;
    EndSceneEntry(BeginSceneEntry(opaque, 0x827F8550, nested), nested);
    EndSceneEntry(c, ctx);
    writerWord(0x6300, 2);
    const auto direct = BeginSceneEntry(opaque, 0x82305648, ctx);
    // Generated frame is 224 bytes; lock output is frame +80/+84. Simulate
    // the original RGB conversion's returned last-region pitch/address pair.
    writerWord(0x1000 - 224 + 80, 16);
    writerWord(0x1000 - 224 + 84, 0xA0001000);
    EndSceneEntry(direct, ctx);

    // sub_82C69850 .135.cpp: owner +36, data/height/width/X step/Y step.
    writerWord(0x7024, 0x7100);
    writerWord(0x7100, 0x8000);
    writerWord(0x7104, 2);
    writerWord(0x7108, 3);
    writerWord(0x710C, 2);
    writerWord(0x7110, 6);
    ctx              = {};
    ctx.r1.u32       = 0x1000;
    ctx.lr           = 0x82C60000;
    ctx.r3.u32       = 0x7000;
    const auto d     = BeginSceneEntry(opaque, 0x82C69850, ctx);
    auto       tile  = ctx;
    tile.lr          = 0x82C69B4C;
    tile.r3.u32      = 0xA0001000;
    tile.r4.u32      = 3;
    tile.r5.u32      = 2;
    tile.r7.u32      = 0x8000;
    tile.r8.u32      = 12;
    tile.r10.u32     = 4;
    const auto tiler = BeginSceneEntry(opaque, 0x826B7E00, tile);
    UNSCOPED_INFO("tiler publishes nothing and leaves enclosing D active");
    REQUIRE(!tiler.session);
    const auto inner     = BeginSceneEntry(opaque, 0x82C69850, ctx);
    const auto innermost = BeginSceneEntry(opaque, 0x82C69850, ctx);
    EndSceneEntry(BeginSceneEntry(opaque, 0x827F8550, nested), nested);
    UNSCOPED_INFO("nested copy adds exactly one span to the innermost writer only");
    REQUIRE((SceneTestWriterRangeCount(innermost) == 1 && SceneTestWriterRangeCount(inner) == 0 && SceneTestWriterRangeCount(d) == 0));
    EndSceneEntry(innermost, ctx);
    UNSCOPED_INFO("child union does not skip its parent to duplicate a grandparent span");
    REQUIRE((SceneTestWriterRangeCount(inner) == 1 && SceneTestWriterRangeCount(d) == 0));
    EndSceneEntry(inner, ctx);
    UNSCOPED_INFO("child forwards its union to the immediate parent once");
    REQUIRE(SceneTestWriterRangeCount(d) == 1);
    // 0001's largest tiled upload (1024x512 RGBA8) has 131,072 central
    // copies. A D writer retains spans but reserves only its single event.
    for (unsigned i = 1; i < 131072; ++i)
    {
        UNSCOPED_INFO("large writer copy burst reserves no child packet");
        REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, nested).session);
    }
    UNSCOPED_INFO("largest observed tile pass fits temporary writer spans");
    REQUIRE(SceneTestWriterRangeCount(d) == 131072);
    EndSceneEntry(tiler, tile);
    EndSceneEntry(d, ctx);

    // sub_82578E00 level wrapper +24 parent; valid source/rect/pitch.
    writerWord(0x6400, 0x40100004);
    writerWord(0x6418, 0x2000);
    writerWord(0x6508, 1);
    writerWord(0x650C, 1);
    ctx               = {};
    ctx.r1.u32        = 0x1000;
    ctx.r3.u32        = 0x6400;
    ctx.r6.u32        = 0x8000;
    ctx.r7.u32        = 6;
    ctx.r8.u32        = 4;
    ctx.r10.u32       = 0x6500;
    const auto upload = BeginSceneEntry(opaque, 0x82578E00, ctx);
    UNSCOPED_INFO("upload retains suppression until return");
    REQUIRE((upload.session && upload.suppressCopies));
    nested.lr = 0x82579360;
    UNSCOPED_INFO("upload child copy publishes nothing");
    REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, nested).session);
    for (unsigned i = 1; i < 131072; ++i)
    {
        UNSCOPED_INFO("large upload copy burst reserves no child packet");
        REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, nested).session);
    }
    UNSCOPED_INFO("upload child tiler publishes nothing");
    REQUIRE(!BeginSceneEntry(opaque, 0x826B7E00, tile).session);
    EndSceneEntry(upload, ctx);

    // Both pre-call decoding and post-call destination reads are optional writer evidence.
    ctx                  = {};
    ctx.r1.u32           = 0x1000;
    ctx.r3.u32           = 0xFFF0;
    const auto badDecode = BeginSceneEntry(opaque, 0x82C69850, ctx);
    EndSceneEntry(badDecode, ctx);
    const auto badRead = BeginSceneEntry(opaque, 0x827F8550, nested);
    UNSCOPED_INFO("upload suppression is popped before the next top-level copy");
    REQUIRE(badRead.session);
    failWriterDestination = true;
    EndSceneEntry(badRead, nested);
    failWriterDestination = false;
    ctx.r3.u32            = 0x2000;
    failWriterRelease     = true;
    BeginSceneEntry(opaque, 0x82697AA8, ctx);
    failWriterRelease = false;
    nested.r5.u32     = 1;
    UNSCOPED_INFO("released texture copy is filtered");
    REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, nested).session);
    await([&]
          {
              try
              {
                  const auto drained = toml::parse_file((directory / "events.toml").string());
                  return drained["events"].as_array() && drained["events"].as_array()->size() == 15;
              }
              catch (const toml::parse_error&)
              {
                  return false;
              }
          });
    // The failed release remains relevant; writer completion is checked separately.
    StopSceneResourceCapture();
    const auto log   = toml::parse_file((directory / "events.toml").string());
    unsigned   paths = 0, copies = 0, uploads = 0, failedWriters = 0;
    for (const auto& node : *log["events"].as_array())
    {
        const auto& row = *node.as_table();
        if (row["kind"].value_or(std::string{}) == "release")
        {
            UNSCOPED_INFO("release fixture really fails decoding while retiring storage");
            REQUIRE((row["read_error"].value_or(std::string{}) == "scene_writer_fixture_unreadable" &&
                     row["failure_relevant"].value_or(false)));
        }
        else if (row["kind"].value_or(std::string{}) != "cpu_writer")
        {
            UNSCOPED_INFO("existing producer fixtures retain successful decoding");
            REQUIRE(!row.contains("read_error"));
        }
        if (row["kind"].value_or(std::string{}) == "upload")
        {
            ++uploads;
            UNSCOPED_INFO("upload remains the proved producer observation");
            REQUIRE((row["object"].value_or(0U) == 0x2000 && !row["raw_target"].value_or(false)));
        }
        if (row["kind"].value_or(std::string{}) != "cpu_writer")
            continue;
        if (row.contains("read_error"))
        {
            ++failedWriters;
            UNSCOPED_INFO("unreadable writer stays open and non-relevant");
            REQUIRE((!row["failure_relevant"].value_or(true) && row["destination_open"].value_or(false)));
            UNSCOPED_INFO("writer decode failure retains path and entry frame");
            REQUIRE((row.contains("entry_frame") && row["writer_path"].value_or(std::string{}) ==
                                                        (row["entry"].value_or(0U) == 0x82C69850 ? "D" : "copy")));
            continue;
        }
        UNSCOPED_INFO("writer retains destination hashes after return");
        REQUIRE((row["destination_ranges"].as_array() && !row["destination_ranges"].as_array()->empty()));
        if (row["entry"].value_or(0U) == 0x827F8550)
        {
            ++copies;
            if (row["source_bytes"].value_or(0U) == 1)
            {
                UNSCOPED_INFO("copy hashes source before Original");
                REQUIRE(row["source_sha256"].value_or(std::string{}) == rex::crypto::sha256("a"));
                UNSCOPED_INFO("copy hashes destination after Original");
                REQUIRE((*row["destination_ranges"].as_array())[0].as_table()->operator[]("destination_sha256").value_or(std::string{}) == rex::crypto::sha256("b"));
            }
        }
        else
        {
            ++paths;
            const auto entry = row["entry"].value_or(0U);
            if (entry == 0x826B5F30)
            {
                UNSCOPED_INFO("A stack arguments and return status");
                REQUIRE((row["raw_stack_64"].value_or(0U) == 0x1234 && row["return_status"].value_or(-1) == 0));
                const auto& ranges = *row["destination_ranges"].as_array();
                UNSCOPED_INFO("A derives exact pitched rows and contiguous source extent");
                REQUIRE((ranges.size() == 2 && (*ranges[0].as_table())["begin"].value_or(0U) == 0x1000 &&
                         (*ranges[1].as_table())["begin"].value_or(0U) == 0x1010 && (*ranges[1].as_table())["bytes"].value_or(0U) == 8 &&
                         row["source_bytes"].value_or(0U) == 16));
            }
            if (entry == 0x82521558)
            {
                UNSCOPED_INFO("B mip source and slice");
                REQUIRE((row["source_bytes"].value_or(0U) == 16 && row["slice"].value_or(1U) == 0));
                UNSCOPED_INFO("B derives the whole-mip destination copy");
                REQUIRE((row["destination_ranges"].as_array()->size() == 1 &&
                         (*(*row["destination_ranges"].as_array())[0].as_table())["bytes"].value_or(0U) == 16));
            }
            if (entry == 0x82305648)
            {
                UNSCOPED_INFO("C backend and 24-byte region records");
                REQUIRE((row["backend"].value_or(0U) == 0x2000 && row["region_records"].as_array()->size() == 1));
                const auto& region = *(*row["region_records"].as_array())[0].as_array();
                UNSCOPED_INFO("C preserves destination XY and source LTRB words with post-call backend");
                REQUIRE((region.size() == 6 && region[0].value_or(0U) == 1 && region[5].value_or(0U) == 6 &&
                         row["backend_after"].value_or(0U) == 0x2000));
            }
            if (entry == 0x82C69850)
            {
                UNSCOPED_INFO("D strided unsigned-16 source envelope");
                REQUIRE(row["source_bytes"].value_or(0U) == 12);
            }
        }
    }
    UNSCOPED_INFO("A-D publish once each; only top-level copies publish; upload children publish nothing");
    REQUIRE((paths == 7 && copies == 2 && uploads == 1 && failedWriters == 2));

    return 0;
}

// The draw window reads a zero G shadow; every other read uses the writer fixture.
std::span<const std::uint8_t> writerDrawMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if (size == guestState.size())
        return guestState;
    if (address == 0x3000 || address == 0x4000 || address == 0x5000)
        return std::span(guestZero).first(size);
    return writerMemory(address, size, physical);
}

// One capture session per process: the unreadable-writer case runs separately.
int writerReadFailureTest()
{
    SetSceneTestMemoryReader(writerDrawMemory);
    writerWord(0x201C, (1U << 22) | 2);
    writerWord(0x2020, 0xA0001006);
    writerWord(0x2024, 31 | (31 << 13));
    writerWord(0x2030, 1U << 9);
    std::string error;
    PPCContext  ctx{};
    PPCContext  nested{};
    nested.r1.u32                = 0x1000;
    nested.r3.u32                = 0xA0001000;
    nested.r4.u32                = 0x8000;
    nested.r5.u32                = 4;
    const auto completeDirectory = (std::filesystem::temp_directory_path() /
                                    ("rerevved-scene-writer-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                                       .string();
    UNSCOPED_INFO("start optional writer-read fixture");
    REQUIRE(StartSceneResourceCapture(completeDirectory, 2, "fixture", "fixture", error));
    auto* opaque                = sceneCaptureSession.load();
    ctx                         = {};
    ctx.r1.u32                  = 0x1000;
    const auto completeCreation = BeginSceneEntry(opaque, 0x82533DB8, ctx);
    ctx.r3.u32                  = 0x2000;
    EndSceneEntry(completeCreation, ctx);
    const auto unreadable = BeginSceneEntry(opaque, 0x827F8550, nested);
    UNSCOPED_INFO("optional writer is admitted");
    REQUIRE(unreadable.session);
    failWriterDestination = true;
    EndSceneEntry(unreadable, nested);
    failWriterDestination = false;
    // Path A sources in 0xC/0xD aliases fall back to the physical heap; an
    // unreadable source is recorded without failing the writer.
    for (const auto sourceAddress : { 0xC0002000U, 0xC0010000U })
    {
        ctx         = {};
        ctx.r1.u32  = 0x1000;
        ctx.lr      = 0x82521AD0;
        ctx.r3.u32  = 0xA0001000;
        ctx.r4.u32  = 16;
        ctx.r5.u32  = 2;
        ctx.r6.u32  = 2;
        ctx.r7.u32  = 6;
        ctx.r9.u32  = sourceAddress;
        ctx.r10.u32 = 8;
        writerWord(0x1054, 6);
        const auto a = BeginSceneEntry(opaque, 0x826B5F30, ctx);
        ctx.r3.u32   = 0;
        EndSceneEntry(a, ctx);
    }
    std::ofstream(std::filesystem::path(completeDirectory) / "arm") << "fixture";
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        NotifySceneResourceFrameBoundary();
        if (SceneTestWindowActive(opaque))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("optional writer fixture arms");
        REQUIRE(attempt != 99);
    }
    ctx.r3.u32 = 0x2000;
    BeginSceneEntry(opaque, 0x8250AC90, ctx);
    BeginSceneEntry(opaque, 0x826A3568, ctx);
    NotifySceneResourceFrameBoundary();
    NotifySceneResourceFrameBoundary();
    await([&]
          {
              return std::filesystem::exists(std::filesystem::path(completeDirectory) / "result.toml");
          });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((std::filesystem::path(completeDirectory) / "result.toml").string());
    UNSCOPED_INFO("one unreadable writer range leaves the full capture complete");
    REQUIRE((result["complete"].value_or(false) && result["read_failures"].value_or(0LL) == 1 &&
             result["relevant_read_failures"].value_or(-1LL) == 0 && result["overflow"].value_or(-1LL) == 0));
    const auto log   = toml::parse_file((std::filesystem::path(completeDirectory) / "events.toml").string());
    unsigned   aRows = 0;
    for (const auto& item : *log["events"].as_array())
    {
        const auto& row = *item.as_table();
        if (row["entry"].value_or(0U) != 0x826B5F30)
            continue;
        ++aRows;
        UNSCOPED_INFO("source failure keeps writer destination evidence");
        REQUIRE((!row.contains("read_error") && !row["destination_open"].value_or(true)));
        if (row["source"].value_or(0U) == 0xC0002000)
        {
            UNSCOPED_INFO("0xC alias source reads through the physical heap");
            REQUIRE((row["source_physical"].value_or(false) && row.contains("source_sha256")));
        }
        else
        {
            UNSCOPED_INFO("unreadable source is recorded separately");
            REQUIRE((row.contains("source_read_error") && !row.contains("source_sha256")));
        }
    }
    UNSCOPED_INFO("both path A writers publish");
    REQUIRE(aRows == 2);
    return 0;
}

constexpr std::uint32_t          payloadGraphicsAddress = 0x100000, payloadVerticesAddress = 0x200000;
std::array<std::uint8_t, 0x4000> payloadGraphics{};
std::array<std::uint8_t, 64>     payloadFormats{};
std::vector<std::uint8_t>        payloadVertices(140000 * 8);
std::vector<std::uint8_t>        payloadCandidate(1024 * 1024);
unsigned                         sampledReads = 0, payloadReads = 0;

std::span<const std::uint8_t> payloadMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    ++payloadReads;
    const auto candidateBase = physical ? 0x10000ULL : address >= 0xA0000000ULL ? 0xA0010000ULL
                                                                                : 0x90000000ULL;
    if (address >= candidateBase && address + size <= candidateBase + payloadCandidate.size())
        return std::span(payloadCandidate).subspan(address - candidateBase, size);
    if (physical && address == 0x1000 && size == 4096)
        ++sampledReads;
    if (!physical && address >= payloadGraphicsAddress && address + size <= payloadGraphicsAddress + payloadGraphics.size())
        return std::span(payloadGraphics).subspan(address - payloadGraphicsAddress, size);
    if (!physical && address >= payloadVerticesAddress && address + size <= payloadVerticesAddress + payloadVertices.size())
        return std::span(payloadVertices).subspan(address - payloadVerticesAddress, size);
    if (address == 0x82091394 && size == 16)
        return std::span(payloadFormats).first(16);
    if (address >= 0x820913A8 && address + size <= 0x820913A8 + payloadFormats.size())
        return std::span(payloadFormats).subspan(address - 0x820913A8, size);
    return writerMemory(address, size, physical);
}

void payloadWord(std::uint32_t address, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        payloadGraphics.at(address + i) = std::uint8_t(value >> (24 - 8 * i));
}

void programWord(unsigned offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        writerPhysical.at(0x3000 + offset + i) = std::uint8_t(value >> (24 - 8 * i));
}

// Textures 0x2000 and 0x2100 over physical 0x1000 and 0x2000, a draw sampling slot 0 at 0x1000,
// and the vertex and shader records the payload draws read.
void setUpPayloadFixture()
{
    SetSceneTestMemoryReader(payloadMemory);
    payloadFormats.fill(1);
    for (const auto object : { 0x2000U, 0x2100U })
    {
        writerWord(object + 0x1C, (1U << 22) | 2);
        writerWord(object + 0x20, object == 0x2000 ? 0xA0001006 : 0xA0002006);
        writerWord(object + 0x24, 31 | (31 << 13));
        writerWord(object + 0x30, 1U << 9);
    }
    payloadWord(0x480, (1U << 22) | 2);
    payloadWord(0x484, 0x1006);
    payloadWord(0x488, 31 | (31 << 13));
    payloadWord(0x494, 1U << 9);
    payloadWord(0x3198, 0x9000);
    payloadWord(0x2E2C, 0x7000);
    payloadWord(0x2948, 1U << 21);
    payloadWord(0x28D8, 0xFFFF);
    writerWord(0x9020, 0x3000);
    writerWord(0x9380, 0x98);
    writerWord(0x9404, 24);
    writerWord(0x941C, 1);
    // A single texture fetch of slot zero, plus one vertex semantic record.
    programWord(0, 1 | 1 << 12 | 1 << 16);
    programWord(4, 0x2000);
    programWord(12, 1);
    writerWord(0x7018, 2);
    writerWord(0x7040, 4);
    writerWord(0x8000, 0x0000FFFF);
    writerWord(0x8004, 0x00010000);
    writerWord(0x1054, 8);
}

int payloadTest(std::string_view mode, const std::filesystem::path& requested)
{
    const bool enabled = mode != "legacy-schema", gaps = mode == "payload-gaps", capacity = mode == "payload-capacity";
    const bool ioFailure = mode == "payload-io-failure";
    const bool preArm    = mode == "payload-pre-arm" || mode == "payload-pre-arm-overflow";
    setUpPayloadFixture();
    const auto  directory = requested.empty() ? std::filesystem::temp_directory_path() /
                                                    ("rerevved-scene-payload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                                              : requested;
    std::string error;
    UNSCOPED_INFO("start payload fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, enabled, 16 * 1024 * 1024, enabled, mode == "payload-targets"));
    auto* opaque = sceneCaptureSession.load();
    if (enabled)
    {
        ObserveSceneAllocation(opaque, false, 0xA0001000, 4096, 0x80000000);
        ObserveSceneAllocation(opaque, false, 0xA0002000, 4096, 0x80000000);
    }
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    for (const auto object : { 0x2000U, 0x2100U })
    {
        const auto token = BeginSceneEntry(opaque, 0x82533DB8, ctx);
        ctx.r3.u32       = object;
        EndSceneEntry(token, ctx);
    }
    PPCContext copy{};
    copy.r1.u32       = 0x1000;
    copy.r3.u32       = 0xA0001000;
    copy.r4.u32       = 0x8100;
    copy.r5.u32       = 4;
    const auto upload = [&](std::uint32_t entry, bool crossFrame)
    {
        PPCContext c{};
        c.r1.u32  = 0x1000;
        c.r3.u32  = 0x2000;
        c.r4.u32  = 0x6000;
        c.r6.u32  = 0x8100;
        c.r7.u32  = 6;
        c.r8.u32  = 4;
        c.r10.u32 = 0x6100;
        writerWord(0x6000, 6);
        writerWord(0x6010, 0x8100);
        writerWord(0x6014, 4);
        writerWord(0x6108, 1);
        writerWord(0x610C, 1);
        const auto token = BeginSceneEntry(opaque, entry, c);
        UNSCOPED_INFO("upload copy remains suppressed");
        REQUIRE(!BeginSceneEntry(opaque, 0x827F8550, copy).session);
        ++writerPhysical[0x1000];
        if (crossFrame)
            NotifySceneResourceFrameBoundary();
        EndSceneEntry(token, c);
    };
    const auto resolve = [&](std::uint32_t object)
    {
        PPCContext c{};
        c.r1.u32  = 0x1000;
        c.r3.u32  = payloadGraphicsAddress;
        c.r6.u32  = object;
        c.r7.u32  = 0x6200;
        c.r10.u32 = 0x6200;
        EndSceneEntry(BeginSceneEntry(opaque, 0x8269E520, c), c);
    };
    upload(0x82578E00, false);
    upload(0x82304CF0, false);
    resolve(0x2100);
    PPCContext boundaryWriter{};
    const auto beginBoundaryWriter = [&]
    {
        writerWord(0x6328, 0x2000);
        writerWord(0x6200, 9);
        writerWord(0x6204, 2);
        writerWord(0x6208, 2);
        writerWord(0x620C, 2);
        writerWord(0x6210, 0x8100);
        writerWord(0x6214, 4);
        for (unsigned i = 0; i < 4; ++i)
            writerWord(0x6500 + i * 4, 0);
        writerWord(0x6510, 2);
        writerWord(0x6514, 2);
        writerWord(0x1850, 32);
        writerWord(0x1854, 0xA0001000);
        boundaryWriter.r1.u32 = 0x1900;
        boundaryWriter.r3.u32 = 0x6300;
        boundaryWriter.r5.u32 = 1;
        boundaryWriter.r6.u32 = 0x6500;
        boundaryWriter.r7.u32 = 0x6200;
        return BeginSceneEntry(opaque, 0x82305648, boundaryWriter);
    };
    if (mode == "payload-region-window")
    {
        const auto token       = beginBoundaryWriter();
        const auto readsBefore = payloadReads;
        // Neither hook may read these invalid addresses or spend the region budget before arm.
        ObserveSceneWriterRegion(opaque, false, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0);
        ObserveSceneWriterRegion(opaque, true, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0);
        UNSCOPED_INFO("non-candidate pre-arm region hooks read no memory");
        REQUIRE(payloadReads == readsBefore);
        EndSceneEntry(token, boundaryWriter);
    }
    if (preArm)
    {
        if (mode == "payload-pre-arm-overflow")
            SetSceneTestPreArmRegionBudget(opaque, 1);
        ObserveSceneAllocation(opaque, false, 0xA0010000, 1024 * 1024, 0x80000000);
        writerWord(0x221C, (32U << 22) | 2);
        writerWord(0x2220, 0xA0010002);
        writerWord(0x2224, 1023 | (1023 << 13));
        writerWord(0x2230, 1U << 9);
        const auto creation = BeginSceneEntry(opaque, 0x82533DB8, ctx);
        ctx.r3.u32          = 0x2200;
        EndSceneEntry(creation, ctx);
        writerWord(0x6328, 0x2200);
        writerWord(0x6200, 9);
        writerWord(0x6204, 1024);
        writerWord(0x6208, 1024);
        writerWord(0x620C, 1024);
        writerWord(0x6210, 0x90000000);
        writerWord(0x6214, 1024 * 1024);
        for (unsigned i = 0; i < 2; ++i)
        {
            writerWord(0x6500 + i * 24, 0);
            writerWord(0x6504 + i * 24, i * 512);
            writerWord(0x6508 + i * 24, 0);
            writerWord(0x650C + i * 24, i * 512);
            writerWord(0x6510 + i * 24, 1024);
            writerWord(0x6514 + i * 24, (i + 1) * 512);
        }
        PPCContext c{};
        c.r1.u32         = 0x1900;
        c.r3.u32         = 0x6300;
        c.r5.u32         = 2;
        c.r6.u32         = 0x6500;
        c.r7.u32         = 0x6200;
        const auto token = BeginSceneEntry(opaque, 0x82305648, c);
        for (unsigned i = 0; i < 2; ++i)
        {
            writerWord(0x1850, 1024);
            writerWord(0x1854, 0xA0010000 + i * 512 * 1024);
            ObserveSceneWriterRegion(opaque, false, 0x1800, 0x6500 + i * 24, 0x6300, 0x6200, 0);
            std::fill_n(payloadCandidate.begin() + i * 512 * 1024, 512 * 1024, std::uint8_t(i + 17));
            ObserveSceneWriterRegion(opaque, true, 0x1800, 0x6500 + i * 24, 0x6300, 0x6200, 0);
        }
        EndSceneEntry(token, c);
    }
    std::ofstream(directory / "arm") << "fixture";
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        NotifySceneResourceFrameBoundary();
        if (SceneTestWindowActive(opaque))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("payload fixture arms");
        REQUIRE(attempt != 99);
    }
    if (ioFailure)
    {
        const auto hash = rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(payloadGraphics.data()), 0x3500));
        await([&]
              {
                  return std::filesystem::exists(directory / "payload-manifest.toml");
              });
        UNSCOPED_INFO("inject payload-only blob I/O failure");
        REQUIRE(std::filesystem::create_directory(directory / "payloads" / (hash + ".bin")));
    }
    const auto draw = [&](bool nested, bool large = false, bool wide = false)
    {
        PPCContext c{};
        c.r1.u32         = 0x1000;
        c.r3.u32         = payloadGraphicsAddress;
        c.r4.u32         = 4;
        c.r5.u32         = large ? 140000 : 0;
        c.r6.u32         = large ? payloadVerticesAddress : 2;
        c.r7.u32         = large ? 8 : 3;
        c.r8.u32         = 0x8000;
        c.r9.u32         = wide ? 4 : 0;
        c.r10.u32        = payloadVerticesAddress;
        const auto token = BeginSceneEntry(opaque, large ? 0x826A3000 : 0x826A3568, c);
        if (nested)
        {
            auto lower    = c;
            lower.r10.u32 = 0x7F00;
            EndSceneEntry(BeginSceneEntry(opaque, 0x826A3048, lower), lower);
        }
        EndSceneEntry(token, c);
    };
    if (preArm)
    {
        payloadWord(0x480, (32U << 22) | 2);
        payloadWord(0x484, 0x10002);
        payloadWord(0x488, 1023 | (1023 << 13));
        draw(false);
        payloadWord(0x480, (1U << 22) | 2);
        payloadWord(0x484, 0x1006);
        payloadWord(0x488, 31 | (31 << 13));
    }
    draw(true);
    const auto reads = sampledReads;
    draw(false);
    UNSCOPED_INFO("unchanged resource version reuses its sampled blob without reading or hashing");
    REQUIRE(sampledReads == reads);
    if (mode == "payload-reuse" || mode == "payload-overlap")
    {
        if (mode == "payload-reuse")
            ObserveSceneAllocation(opaque, true, 0xA0001000, 0, 0x80000000);
        ObserveSceneAllocation(opaque, false, 0xA0001000, 4096, 0x80000000);
        ++writerPhysical[0x1000];
        draw(false);
        UNSCOPED_INFO("same descriptor in a new allocation must reread bytes");
        REQUIRE(sampledReads > reads);
    }
    if (mode == "payload-regions" || mode == "payload-region-failure" || mode == "payload-regions-copy")
    {
        writerWord(0x6328, 0x2000);
        writerWord(0x6200, mode == "payload-regions-copy" ? 1 : 2);
        writerWord(0x6204, 2);
        writerWord(0x6208, 2);
        writerWord(0x620C, mode == "payload-regions-copy" ? 8 : 6);
        writerWord(0x6210, 0x8100);
        writerWord(0x6214, mode == "payload-regions-copy" ? 16 : 12);
        for (unsigned i = 0; i < 2; ++i)
        {
            writerWord(0x6500 + i * 24, i * 2);
            writerWord(0x6510 + i * 24, 2);
            writerWord(0x6514 + i * 24, 2);
        }
        PPCContext c{};
        c.r1.u32         = 0x1900;
        c.r3.u32         = 0x6300;
        c.r5.u32         = 2;
        c.r6.u32         = 0x6500;
        c.r7.u32         = 0x6200;
        const auto token = BeginSceneEntry(opaque, 0x82305648, c);
        for (unsigned i = 0; i < 2; ++i)
        {
            writerWord(0x1850, 32);
            writerWord(0x1854, 0xA0001000 + i * 8);
            ObserveSceneWriterRegion(opaque, false, 0x1800, 0x6500 + i * 24, 0x6300, 0x6200, 1);
            writerPhysical[0x1000 + i * 8] = std::uint8_t(i + 17);
            if (mode != "payload-region-failure" || i == 0)
                ObserveSceneWriterRegion(opaque, true, 0x1800, 0x6500 + i * 24, 0x6300, 0x6200, 1);
        }
        EndSceneEntry(token, c);
    }
    writerWord(0x8000, 0);
    writerWord(0x8004, UINT32_MAX);
    writerWord(0x8008, 1);
    payloadWord(0x28D8, 0xFFFFFF);
    draw(false, false, true);
    payloadWord(0x28D8, 0xFFFF);
    writerWord(0x8000, 0x0000FFFF);
    writerWord(0x8004, 0x00010000);
    if (mode == "payload-coalescing")
        draw(false, true);
    if (mode == "payload-wide-stride")
    {
        // Element 1 is declared but unused, and the stride leaves gaps between vertices.
        writerWord(0x7048, 0x00020300);
        writerWord(0x1054, 64);
        draw(false);
        writerWord(0x1054, 8);
    }
    if (mode == "payload-allocator")
    {
        writerWord(0x7400, 1U << 29);
        writerWord(0x7418, 0x3800);
        std::copy_n(writerGuest.begin() + 0x8000, 8, writerPhysical.begin() + 0x3800);
        payloadWord(0x3094, 0x7400);
        payloadWord(0x778, 0x1003);
        payloadWord(0x77C, 4U << 2);
        payloadGraphics[0x30F0] = 2;
        PPCContext bound{};
        bound.r1.u32 = 0x1000;
        bound.r3.u32 = payloadGraphicsAddress;
        bound.r7.u32 = 3;
        EndSceneEntry(BeginSceneEntry(opaque, 0x826A39F8, bound), bound);
    }

    // A real writer enclosing an upload must not acquire that upload's suppressed copies.
    PPCContext writer{};
    writer.r1.u32  = 0x1000;
    writer.r3.u32  = 0xA0001020;
    writer.r4.u32  = 4;
    writer.r5.u32  = 1;
    writer.r6.u32  = 1;
    writer.r7.u32  = 6;
    writer.r9.u32  = 0x8100;
    writer.r10.u32 = 4;
    writerWord(0x1054, 6);
    const auto parentWriter = BeginSceneEntry(opaque, 0x826B5F30, writer);
    upload(0x82578E00, false);
    writer.r3.u32 = 0;
    EndSceneEntry(parentWriter, writer);
    writerWord(0x1054, 8);
    upload(0x82304CF0, true);
    draw(false);
    if (enabled)
    {
        UNSCOPED_INFO("observed writes invalidate the sampled texture cache");
        REQUIRE(sampledReads > reads);
    }
    resolve(0x2100);
    if (gaps)
    {
        payloadWord(0x2948, 0);
        draw(false);
        payloadWord(0x2948, 1U << 21);
        payloadWord(0x484, 0x5006);
        draw(false);
        // Type 2 with a forbidden tiled 1D layout must retain its demand row.
        payloadWord(0x480, (1U << 31) | (1U << 22) | 2);
        payloadWord(0x494, 0);
        draw(false);
        programWord(0, 2 | 1 << 12 | 1 << 16);
        draw(false);
        PPCContext direct{};
        direct.r1.u32  = 0x1000;
        direct.r3.u32  = payloadGraphicsAddress;
        direct.r10.u32 = 0x7F00;
        EndSceneEntry(BeginSceneEntry(opaque, 0x826A3048, direct), direct);
        resolve(0x2200);
    }
    if (capacity)
    {
        std::vector<SceneHookToken> inflight;
        for (unsigned i = 0; i < 64; ++i)
            inflight.push_back(BeginSceneEntry(opaque, 0x82533DB8, ctx));
        UNSCOPED_INFO("capacity closes the scene window");
        REQUIRE((!BeginSceneEntry(opaque, 0x82533DB8, ctx).session && !SceneTestWindowActive(opaque)));
        payloadReads      = 0;
        ctx.r3.u32        = payloadGraphicsAddress;
        const auto reader = BeginRouteEffectReader(0x8269BE70, ctx);
        EndRouteEffectReader(reader);
        UNSCOPED_INFO("capacity close stops effect progress reads");
        REQUIRE(!payloadReads);
        ctx.r3.u32 = 0x2000;
        for (auto token = inflight.rbegin(); token != inflight.rend(); ++token)
            EndSceneEntry(*token, ctx);
    }
    else if (mode == "payload-region-window")
    {
        const auto token = beginBoundaryWriter();
        ObserveSceneWriterRegion(opaque, false, 0x1800, 0x6500, 0x6300, 0x6200, 0);
        NotifySceneResourceFrameBoundary();
        ObserveSceneWriterRegion(opaque, true, 0x1800, 0x6500, 0x6300, 0x6200, 0);
        EndSceneEntry(token, boundaryWriter);
    }
    else
        NotifySceneResourceFrameBoundary();
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("payload gaps leave metadata completion independent");
    REQUIRE((result["complete"].value_or(false) == !capacity && result["relevant_read_failures"].value_or(-1LL) == 0));
    if (enabled)
    {
        UNSCOPED_INFO("payload completeness is independent of allocation and region coverage");
        REQUIRE(result["payload_complete"].value_or(false) == (!gaps && !capacity && !ioFailure));
        UNSCOPED_INFO("effect counters are exposed in the result");
        REQUIRE(result["effect_complete"].value_or(false) == !capacity);
        UNSCOPED_INFO("allocator lifetime gaps are separate from retention failures");
        REQUIRE((result["payload_allocator_gaps"].value_or(0ULL) > 0) == (mode == "payload-allocator"));
        if (mode == "payload-targets")
        {
            UNSCOPED_INFO("one opening seed request and two closing references stay explicitly unavailable");
            REQUIRE((!result["target_complete"].value_or(true) && result["target_gaps"].value_or(0) == 3 &&
                     result["target_records"].value_or(0) == 3));
        }
        if (mode == "payload-regions" || mode == "payload-regions-copy")
        {
            UNSCOPED_INFO("every writer C region retained");
            REQUIRE((result["region_records"].value_or(0) == 2 && result["region_failures"].value_or(-1) == 0));
        }
        if (mode == "payload-region-failure")
        {
            UNSCOPED_INFO("missing region is a separate coverage failure");
            REQUIRE((result["region_records"].value_or(0) == 1 && result["region_failures"].value_or(-1) == 1));
        }
        if (mode == "payload-region-window")
        {
            UNSCOPED_INFO("pre-arm and boundary-straddling regions spend no budget and cause no failure");
            REQUIRE((result["region_records"].value_or(-1) == 0 && result["region_failures"].value_or(-1) == 0));
            UNSCOPED_INFO("non-candidate pre-arm regions consume no history budget");
            REQUIRE((result["pre_arm_region_records"].value_or(-1) == 0 && result["pre_arm_region_failures"].value_or(-1) == 0 && result["pre_arm_region_overflow"].value_or(-1) == 0));
        }
        if (preArm)
        {
            const bool overflow = mode == "payload-pre-arm-overflow";
            UNSCOPED_INFO("candidate pre-arm regions use their own bounded budget");
            REQUIRE((result["pre_arm_region_records"].value_or(-1) == (overflow ? 1 : 2) && result["pre_arm_region_overflow"].value_or(-1) == (overflow ? 1 : 0)));
            UNSCOPED_INFO("pre-arm regions leave the window budget untouched");
            REQUIRE((result["region_records"].value_or(-1) == 0 && result["region_failures"].value_or(-1) == 0));
        }
        if (mode == "payload-reuse" || mode == "payload-overlap")
        {
            const auto              manifest = toml::parse_file((directory / "payload-manifest.toml").string());
            std::set<std::uint64_t> generations;
            for (const auto& row : *manifest["bindings"].as_array())
                if ((*row.as_table())["role"].value_or("") == std::string_view("sampled_base"))
                    generations.insert((*row.as_table())["allocation_generation"].value_or(0ULL));
            UNSCOPED_INFO("payload manifest distinguishes the observed allocate/free/reallocate cycle");
            REQUIRE((generations == std::set<std::uint64_t>{ 1, 3 } && result["allocation_free_calls"].value_or(0) == (mode == "payload-reuse" ? 1 : 0)));
            UNSCOPED_INFO("overlap recovery has a separate result counter");
            REQUIRE((result["allocation_overlaps"].value_or(-1) == (mode == "payload-overlap" ? 1 : 0) && result["allocation_overflow"].value_or(-1) == 0));
        }
        if (mode == "payload-wide-stride")
        {
            // Vertices 0 and 1 at stride 64 retain one covering span (68 bytes), not one row per vertex.
            const auto                        manifest = toml::parse_file((directory / "payload-manifest.toml").string());
            std::map<std::uint64_t, unsigned> rowsByEvent;
            std::uint64_t                     wideEvent = 0;
            for (const auto& item : *manifest["bindings"].as_array())
            {
                const auto& row = *item.as_table();
                if (row["role"].value_or(std::string_view{}) != "vertices")
                    continue;
                ++rowsByEvent[row["event"].value_or(0ULL)];
                if (row["bytes"].value_or(0ULL) == 68)
                    wideEvent = row["event"].value_or(0ULL);
            }
            UNSCOPED_INFO("wide-stride vertices retain one covering range per stream");
            REQUIRE((wideEvent && rowsByEvent[wideEvent] == 1));
        }
        if (ioFailure)
        {
            UNSCOPED_INFO("payload I/O failure is reported independently");
            REQUIRE(!result["payload_failure_reason"].value_or(std::string{}).empty());
        }
    }
    else
    {
        UNSCOPED_INFO("payload-off output retains the legacy schema");
        REQUIRE((!result.contains("payload_complete") && !result.contains("effect_file") && !std::filesystem::exists(directory / "payload-manifest.toml")));
    }
    const auto              log = toml::parse_file((directory / "events.toml").string());
    std::set<std::uint64_t> sequences;
    unsigned                returns = 0, bootEvents = 0;
    for (const auto& item : *log["events"].as_array())
    {
        const auto& row = *item.as_table();
        UNSCOPED_INFO("every row has a distinct global identity");
        REQUIRE(sequences.insert(row["sequence"].value_or(0ULL)).second);
        const auto kind = row["kind"].value_or(std::string_view{});
        if (row["frame"].value_or(0ULL) < result["begin_frame"].value_or(0ULL))
            ++bootEvents;
        if (kind == "upload_return" || kind == "resolve_return")
            ++returns;
        if (kind == "cpu_writer")
        {
            if (row["writer_path"].value_or("") == std::string_view("C"))
            {
                const auto* regions = row["region_updates"].as_array();
                if (preArm)
                {
                    UNSCOPED_INFO("candidate region metadata survives until publication");
                    REQUIRE((row["pre_arm_regions"].value_or(false) && regions && regions->size() == (mode == "payload-pre-arm-overflow" ? 1 : 2)));
                    continue;
                }
                if (mode == "payload-region-window")
                {
                    UNSCOPED_INFO("out-of-window regions are not retained");
                    REQUIRE((!regions || regions->empty()));
                    continue;
                }
                UNSCOPED_INFO("writer C serializes completed regions");
                REQUIRE((regions && regions->size() == (mode == "payload-region-failure" ? 1 : 2)));
                for (const auto& region : *regions)
                {
                    UNSCOPED_INFO("direct fallback rows retained at each unlock");
                    REQUIRE(((*region.as_table())["fallback"].value_or(false) == (mode != "payload-regions-copy") && (*region.as_table())["rows_written"].value_or(0) == 2 && (*region.as_table())["rows"].as_array()->size() == 2));
                }
                continue;
            }
            const auto* ranges = row["destination_ranges"].as_array();
            const auto* first  = ranges && ranges->size() == 1 ? (*ranges)[0].as_table() : nullptr;
            UNSCOPED_INFO("payload upload copies never change legacy writer ranges");
            REQUIRE((first && (*first)["guest_address"].value_or(0U) == 0xA0001020 && (*first)["bytes"].value_or(0U) == 4));
        }
        if (!enabled)
        {
            UNSCOPED_INFO("legacy events contain no extension fields");
            REQUIRE((!row.contains("capture_id") && !row.contains("call_id") && !row.contains("payload_texture_uses")));
        }
    }
    if (mode != "payload-region-window")
    {
        UNSCOPED_INFO("pre-arm candidate creation and writer events are retained");
        REQUIRE(bootEvents == (enabled ? 9 : 7) + (preArm ? 4 : 0));
    }
    // Two armed uploads (including the nested upload) plus one resolve; gaps adds one resolve.
    UNSCOPED_INFO("only armed uploads and resolves have paired return rows");
    REQUIRE(returns == (enabled ? (gaps ? 4 : 3) : 0));
    return 0;
}

// Payload windows use own allocations after late texture-0 reallocation.
int payloadWindowsTest(const std::filesystem::path& requested)
{
    setUpPayloadFixture();
    const auto  directory = requested.empty() ? std::filesystem::temp_directory_path() /
                                                    ("rerevved-scene-payload-windows-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                                              : requested;
    std::string error;
    UNSCOPED_INFO("start payload windows fixture");
    REQUIRE(StartSceneResourceCapture(directory, 1, "fixture", "fixture", error, true, 16 * 1024 * 1024, false, false, 0, 1, { "", 0, 2 }));
    auto* first = sceneCaptureSession.load();
    ObserveSceneAllocation(first, false, 0xA0001000, 4096, 0x80000000);
    ObserveSceneAllocation(first, false, 0xA0002000, 4096, 0x80000000);
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    for (const auto object : { 0x2000U, 0x2100U })
    {
        const auto token = BeginSceneEntry(first, 0x82533DB8, ctx);
        ctx.r3.u32       = object;
        EndSceneEntry(token, ctx);
    }
    const auto draw = [](void* opaque, std::uint32_t fetchBase)
    {
        payloadWord(0x484, fetchBase);
        PPCContext c{};
        c.r1.u32  = 0x1000;
        c.r3.u32  = payloadGraphicsAddress;
        c.r4.u32  = 4;
        c.r6.u32  = 2;
        c.r7.u32  = 3;
        c.r8.u32  = 0x8000;
        c.r10.u32 = payloadVerticesAddress;
        EndSceneEntry(BeginSceneEntry(opaque, 0x826A3568, c), c);
        payloadWord(0x484, 0x1006);
    };
    NotifySceneResourceFrameBoundary();
    UNSCOPED_INFO("window 1 opens on arm_frame");
    REQUIRE(SceneTestWindowActive(first));
    draw(first, 0x1006);
    // A path A writer into texture 0x2100 that returns after window 1 closed.
    PPCContext writer{};
    writer.r1.u32  = 0x1000;
    writer.r3.u32  = 0xA0002020;
    writer.r4.u32  = 4;
    writer.r5.u32  = 1;
    writer.r6.u32  = 1;
    writer.r7.u32  = 6;
    writer.r9.u32  = 0x8100;
    writer.r10.u32 = 4;
    writerWord(0x1054, 6);
    const auto straddling = BeginSceneEntry(first, 0x826B5F30, writer);
    UNSCOPED_INFO("the writer is observed in window 1");
    REQUIRE(straddling.session != nullptr);
    NotifySceneResourceFrameBoundary();
    auto* second = sceneCaptureSession.load();
    UNSCOPED_INFO("window 1 closes into the window 2 session");
    REQUIRE((second && second != first && !SceneTestWindowActive(first)));
    ++writerPhysical[0x2020];
    writer.r3.u32 = 0;
    EndSceneEntry(straddling, writer);
    writerWord(0x1054, 8);
    // Window 1's hook still delivers these; window 2 must log them.
    ObserveSceneAllocation(first, true, 0xA0002000, 0, 0x80000000);
    ObserveSceneAllocation(first, false, 0xA0002000, 4096, 0x80000000);
    std::ofstream(directory / "arm-2") << "fixture";
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        NotifySceneResourceFrameBoundary();
        if (SceneTestWindowActive(second))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("window 2 arms");
        REQUIRE(attempt != 99);
    }
    draw(second, 0x1006);
    draw(second, 0x2006);
    NotifySceneResourceFrameBoundary();
    UNSCOPED_INFO("the last window leaves no successor");
    REQUIRE(!sceneCaptureSession.load());
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml") && std::filesystem::exists(directory / "window-2" / "result.toml");
          });
    StopSceneResourceCapture();
    for (const auto& window : { directory, directory / "window-2" })
    {
        const auto result = toml::parse_file((window / "result.toml").string());
        UNSCOPED_INFO("each window retains complete payloads of its own");
        REQUIRE((result["complete"].value_or(false) && result["payload_complete"].value_or(false) && result["payload_failures"].value_or(-1) == 0 &&
                 result["payload_allocator_gaps"].value_or(-1) == 0));
    }
    const auto result = toml::parse_file((directory / "window-2" / "result.toml").string());
    UNSCOPED_INFO("window 2 opens with the live allocations and logs the closed window's later calls");
    REQUIRE((result["allocation_opening_records"].value_or(-1) == 2 && result["allocation_calls"].value_or(-1) == 1 && result["allocation_free_calls"].value_or(-1) == 1));
    const auto log     = toml::parse_file((directory / "window-2" / "events.toml").string());
    const auto opening = *(*log["events"].as_array())[0].as_table();
    UNSCOPED_INFO("the opening snapshot is window 2's first event");
    REQUIRE((opening["kind"].value_or(std::string_view{}) == "allocation_snapshot" && opening["sequence"].value_or(0) == 1 &&
             opening["allocations"].as_array()->size() == 2));
    return 0;
}

int recorderTest(bool shutdown)
{
    SetSceneTestMemoryReader(guestMemory);
    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start recorder fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error));
    auto*      opaque = sceneCaptureSession.load();
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    if (shutdown)
    {
        const auto token = BeginSceneEntry(opaque, 0x82533DB8, ctx);
        UNSCOPED_INFO("creation retains an inflight return observer");
        REQUIRE(token.session != nullptr);
        StopSceneResourceCapture();
        const auto result = toml::parse_file((directory / "result.toml").string());
        UNSCOPED_INFO("shutdown publishes failure without waiting for parked creation");
        REQUIRE((!result["complete"].value_or(true) && result["inflight_at_result"].value_or(0LL) == 1));
        EndSceneEntry(token, ctx);
        return 0;
    }
    for (auto entry : { 0x8268FB08U, 0x8268FCB0U, 0x826A4638U, 0x8250AC90U, 0x826A3568U })
    {
        UNSCOPED_INFO("window-only events are gated before arm");
        REQUIRE(!BeginSceneEntry(opaque, entry, ctx).session);
    }
    // Null arguments return E_INVALIDARG in the guest; no reads are attempted.
    EndSceneEntry(BeginSceneEntry(opaque, 0x82578E00, ctx), ctx);
    // The decoded input's source_bytes is zero and its source address is null.
    ctx.r4.u32 = 0x3000;
    EndSceneEntry(BeginSceneEntry(opaque, 0x82304CF0, ctx), ctx);
    // Raw direct upload target with a zero-pitch/zero-rectangle source span.
    ctx.r3.u32  = 0x4000;
    ctx.r6.u32  = 0x7000;
    ctx.r7.u32  = 6;
    ctx.r10.u32 = 0x5000;
    EndSceneEntry(BeginSceneEntry(opaque, 0x82578E00, ctx), ctx);
    await([&]
          {
              return std::filesystem::exists(directory / "status.toml");
          });
    std::ofstream(directory / "arm") << "fixture";
    ctx.r3.u32 = 0x2000;
    // Notify until the worker has polled arm, then hold the window for the draw.
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        NotifySceneResourceFrameBoundary();
        if (SceneTestWindowActive(opaque))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("arm starts a complete frame window");
        REQUIRE(attempt != 99);
    }
    BeginSceneEntry(opaque, 0x8250AC90, ctx);
    // A non-null PS object whose read throws used to leave state without bindings.
    guestState[0x3196] = 0x60;
    BeginSceneEntry(opaque, 0x826A3568, ctx);
    await([&]
          {
              try
              {
                  const auto status = toml::parse_file((directory / "status.toml").string());
                  return status["relevant_read_failures"].value_or(0LL) == 1;
              }
              catch (const toml::parse_error&)
              {
                  return false;
              }
          });
    NotifySceneResourceFrameBoundary();
    NotifySceneResourceFrameBoundary();
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("shader read failure is recorded without crashing worker");
    REQUIRE((!result["complete"].value_or(true) && result["read_failures"].value_or(0LL) == 1));
    const auto log  = toml::parse_file((directory / "events.toml").string());
    const auto rows = log["events"];
    UNSCOPED_INFO("boot window-only events consume no event budget");
    REQUIRE((rows.is_array() && rows.as_array()->size() == 5));
    UNSCOPED_INFO("guest early-out is not a read failure");
    REQUIRE(rows[0]["kind"].value_or(std::string{}) == "rejected_by_guest");
    constexpr auto emptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    UNSCOPED_INFO("size-zero spans produce the SHA-256 of empty bytes");
    REQUIRE((rows[1]["source_sha256"].value_or(std::string{}) == emptyHash && rows[2]["source_sha256"].value_or(std::string{}) == emptyHash));
    UNSCOPED_INFO("unproved direct upload target is flagged");
    REQUIRE(rows[2]["raw_target"].value_or(false));
    UNSCOPED_INFO("failed draw retains error and never enters binding serialization");
    REQUIRE((rows[4]["read_error"].value_or(std::string{}) == "scene_test_shader_read" && !rows[4].as_table()->contains("state_file")));
    return 0;
}

// The arm file and arm_frame are independent triggers. Each case is one process: a session is a singleton.
int armTest(std::string_view mode)
{
    const bool          off = mode == "arm-off", fileFirst = mode == "arm-file-first", frameFirst = mode == "arm-frame-first";
    const std::uint32_t armFrame  = off ? 0 : mode == "arm-frame" || fileFirst ? 5
                                                                               : 3;
    const auto          directory = std::filesystem::temp_directory_path() /
                                    ("rerevved-scene-arm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string         error;
    UNSCOPED_INFO("start arm fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, false, 0, false, false, 0, armFrame));
    auto* opaque = sceneCaptureSession.load();
    if (fileFirst)
    {
        std::ofstream(directory / "arm") << "fixture";
        await([&]
              {
                  return SceneTestArmed(opaque);
              });
    }
    // Frame numbers start at 1: the n-th boundary call is frame n.
    const auto boundary = [&]
    {
        NotifySceneResourceFrameBoundary();
    };
    if (off)
    {
        for (unsigned i = 0; i < 10; ++i)
            boundary();
        UNSCOPED_INFO("arm_frame 0 and no file never arms");
        REQUIRE((!SceneTestArmed(opaque) && !SceneTestWindowActive(opaque)));
    }
    else if (fileFirst)
    {
        boundary();
        UNSCOPED_INFO("the arm file opens the window at the next boundary, before arm_frame");
        REQUIRE(SceneTestWindowActive(opaque));
        boundary();
        boundary();
    }
    else
    {
        for (unsigned i = 1; i < armFrame; ++i)
        {
            boundary();
            UNSCOPED_INFO("arm_frame does not arm early");
            REQUIRE((!SceneTestWindowActive(opaque) && !SceneTestArmed(opaque)));
        }
        boundary();
        UNSCOPED_INFO("arm_frame opens the window at its own boundary");
        REQUIRE(SceneTestWindowActive(opaque));
        if (frameFirst)
            std::ofstream(directory / "arm") << "fixture";
        boundary();
        boundary();
    }
    if (!off)
        await([&]
              {
                  return std::filesystem::exists(directory / "result.toml");
              });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("result records arm_frame");
    REQUIRE(result["arm_frame"].value_or(-1) == armFrame);
    if (off)
    {
        UNSCOPED_INFO("an unarmed capture records no window");
        REQUIRE((result["begin_frame"].value_or(-1) == 0 && !result["complete"].value_or(true)));
        return 0;
    }
    const std::int64_t begin = fileFirst ? 1 : armFrame;
    // Without draws the window closes but is not complete; only its bounds are under test.
    UNSCOPED_INFO("the first trigger fixes the window and a later one cannot move it");
    REQUIRE((result["begin_frame"].value_or(-1) == begin && result["end_frame"].value_or(-1) == begin + 2));
    return 0;
}

// Draw-triggered arming. G is at 0; the pixel shader object is at 0x4000 and its microcode at
// physical 0x100, in the layout sub_826ADD88 gives the shader metadata that locateShader reads.
std::array<std::uint8_t, 0x5000> triggerGuest{};
std::array<std::uint8_t, 0x200>  triggerPhysical{};

std::span<const std::uint8_t> triggerMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if (physical && address + size <= triggerPhysical.size())
        return std::span(triggerPhysical).subspan(address, size);
    if (!physical && address + size <= triggerGuest.size())
        return std::span(triggerGuest).subspan(address, size);
    throw std::runtime_error("scene_trigger_fixture_extent");
}

void triggerWord(std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        triggerGuest[offset + i] = std::uint8_t(value >> (24 - 8 * i));
}

constexpr std::array<std::uint8_t, 16> triggerShaderA{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
constexpr std::array<std::uint8_t, 16> triggerShaderB{ 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

void setUpTriggerFixture()
{
    SetSceneTestMemoryReader(triggerMemory);
    triggerWord(0x3194, 0x4000);
    triggerWord(0x4000 + 24, 0x100);
    triggerWord(0x4000 + 64, 0x100);
    triggerWord(0x4000 + 40 + 0x100 + 4, 16);
}

// One draw through the hook entry the recorder watches; the pixel shader is whichever code sits at physical 0x100.
void triggerDraw(void* opaque, const std::array<std::uint8_t, 16>& code)
{
    std::copy(code.begin(), code.end(), triggerPhysical.begin() + 0x100);
    PPCContext ctx{};
    ctx.r1.u32       = 0x1000;
    ctx.lr           = 0x8250C768;
    const auto token = BeginSceneEntry(opaque, 0x826A3568, ctx);
    UNSCOPED_INFO("a draw outside the window records nothing");
    REQUIRE(!token.session);
}

std::string triggerDigest(const std::array<std::uint8_t, 16>& code)
{
    return ScenePixelWordDigest(code);
}

// The first draw with a matching pixel shader opens the window armDelay + 1 boundaries after its frame.
int digestArmTest(std::string_view mode)
{
    setUpTriggerFixture();
    const bool          next  = mode == "arm-digest-next";
    const std::uint32_t delay = next ? 0 : 1;
    // arm-digest-next also proves the cvar value is case-insensitive.
    std::string prefix = triggerDigest(triggerShaderA).substr(0, 12), shown = prefix;
    if (next)
    {
        shown = prefix.substr(0, 8);
        for (auto& c : shown)
            c = std::toupper(static_cast<unsigned char>(c));
    }
    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-digest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start digest fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, false, 0, false, false, 0, 0, { shown, delay, 1 }));
    auto*      opaque   = sceneCaptureSession.load();
    const auto boundary = [&]
    {
        NotifySceneResourceFrameBoundary();
    };
    boundary();
    boundary();
    triggerDraw(opaque, triggerShaderB);
    boundary();
    UNSCOPED_INFO("a different pixel shader does not arm");
    REQUIRE(!SceneTestWindowActive(opaque));
    triggerDraw(opaque, triggerShaderA);
    // The hit is frame 3; later matching draws cannot move the window.
    triggerDraw(opaque, triggerShaderA);
    std::uint32_t frame = 3;
    for (std::uint32_t i = 0; i < delay; ++i)
    {
        boundary();
        ++frame;
        UNSCOPED_INFO("the delay holds the window back");
        REQUIRE(!SceneTestWindowActive(opaque));
    }
    boundary();
    ++frame;
    UNSCOPED_INFO("the window opens armDelay + 1 boundaries after the matching frame");
    REQUIRE(SceneTestWindowActive(opaque));
    boundary();
    boundary();
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("the digest hit fixes the window");
    REQUIRE((result["begin_frame"].value_or(-1) == frame && result["end_frame"].value_or(-1) == frame + 2));
    UNSCOPED_INFO("the result records the digest trigger");
    REQUIRE((result["trigger"].value_or(std::string{}) == "pixel_digest" && result["arm_delay"].value_or(-1) == delay &&
             result["arm_pixel_digest"].value_or(std::string{}) == (next ? prefix.substr(0, 8) : prefix)));
    return 0;
}

std::atomic<bool> raceBlock{ false }, raceEntered{ false }, raceRelease{ false };

// Holds the matching draw inside its shader read until the test releases it.
std::span<const std::uint8_t> raceMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if (physical && address == 0x100 && raceBlock.exchange(false))
    {
        raceEntered = true;
        while (!raceRelease)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return triggerMemory(address, size, physical);
}

// Boundary-crossing shader reads keep draw frame; capture opens next boundary.
int digestRaceTest()
{
    setUpTriggerFixture();
    SetSceneTestMemoryReader(raceMemory);
    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-digest-race-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start digest race fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, false, 0, false, false, 0, 0, { triggerDigest(triggerShaderA).substr(0, 12), 0, 1 }));
    auto* opaque = sceneCaptureSession.load();
    NotifySceneResourceFrameBoundary();
    NotifySceneResourceFrameBoundary();
    raceBlock = true;
    std::thread drawing([&]
                        {
                            triggerDraw(opaque, triggerShaderA);
                        });
    await([&]
          {
              return raceEntered.load();
          });
    // The draw began in frame 2; this boundary ends frame 2 and must order after the hit.
    std::thread boundary([]
                         {
                             NotifySceneResourceFrameBoundary();
                         });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    raceRelease = true;
    drawing.join();
    boundary.join();
    UNSCOPED_INFO("the boundary after the draw's frame opens the window");
    REQUIRE(SceneTestWindowActive(opaque));
    NotifySceneResourceFrameBoundary();
    NotifySceneResourceFrameBoundary();
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    StopSceneResourceCapture();
    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("the hit keeps its draw's frame");
    REQUIRE((result["begin_frame"].value_or(-1) == 3 && result["trigger"].value_or(std::string{}) == "pixel_digest"));
    return 0;
}

// Windows k >= 2 open on their own arm-<k> file and write window-<k>; the capture directory keeps window 1.
int windowsTest()
{
    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-windows-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start windows fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, true, 16 * 1024 * 1024, false, false, 0, 2, { "", 0, 3 }));
    auto*      first    = sceneCaptureSession.load();
    const auto boundary = [&]
    {
        NotifySceneResourceFrameBoundary();
    };
    boundary();
    UNSCOPED_INFO("arm_frame does not arm early");
    REQUIRE(!SceneTestWindowActive(first));
    boundary();
    UNSCOPED_INFO("window 1 opens on arm_frame");
    REQUIRE(SceneTestWindowActive(first));
    boundary();
    boundary();
    auto* second = sceneCaptureSession.load();
    UNSCOPED_INFO("window 1 closes into the window 2 session");
    REQUIRE((second && second != first && std::filesystem::is_directory(directory / "window-2")));
    // Only arm-2 opens window 2, however early arm-3 appears.
    std::ofstream(directory / "arm-3") << "fixture";
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    boundary();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    UNSCOPED_INFO("arm-3 does not open window 2");
    REQUIRE((!SceneTestArmed(second) && !SceneTestWindowActive(second)));
    std::ofstream(directory / "arm-2") << "fixture";
    await([&]
          {
              return SceneTestArmed(second);
          });
    boundary();
    UNSCOPED_INFO("arm-2 opens window 2 at the next boundary");
    REQUIRE(SceneTestWindowActive(second));
    boundary();
    boundary();
    auto* third = sceneCaptureSession.load();
    UNSCOPED_INFO("window 2 closes into the window 3 session");
    REQUIRE((third && third != second && std::filesystem::is_directory(directory / "window-3")));
    await([&]
          {
              return SceneTestArmed(third);
          });
    boundary();
    UNSCOPED_INFO("the waiting arm-3 opens window 3");
    REQUIRE(SceneTestWindowActive(third));
    boundary();
    boundary();
    UNSCOPED_INFO("the last window leaves no successor");
    REQUIRE((!sceneCaptureSession.load() && !std::filesystem::exists(directory / "window-4")));
    await([&]
          {
              return std::filesystem::exists(directory / "window-2" / "result.toml") && std::filesystem::exists(directory / "window-3" / "result.toml");
          });
    StopSceneResourceCapture();

    struct Expected
    {
        std::filesystem::path result;
        std::int64_t          window, begin, end;
        const char*           trigger;
    };

    std::set<std::string> captureIds;
    for (const auto& e : { Expected{ directory / "result.toml", 1, 2, 4, "arm_frame" }, Expected{ directory / "window-2" / "result.toml", 2, 6, 8, "file" }, Expected{ directory / "window-3" / "result.toml", 3, 9, 11, "file" } })
    {
        const auto result = toml::parse_file(e.result.string());
        UNSCOPED_INFO("each window records its own bounds and trigger");
        REQUIRE((result["window"].value_or(-1) == e.window && result["begin_frame"].value_or(-1) == e.begin && result["end_frame"].value_or(-1) == e.end &&
                 result["trigger"].value_or(std::string{}) == e.trigger));
        UNSCOPED_INFO("each window writes its own events");
        REQUIRE(std::filesystem::exists(e.result.parent_path() / "events.toml"));
        const auto id = result["capture_id"].value_or(std::string{});
        UNSCOPED_INFO("each window has its own payload capture identity");
        REQUIRE((!id.empty() && captureIds.insert(id).second));
    }
    return 0;
}

// Bad trigger values never start a session, so the same process can try again.
int digestRefusedTest()
{
    const auto  root = std::filesystem::temp_directory_path() /
                       ("rerevved-scene-refused-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    int         attempt = 0;
    const auto  start   = [&](const SceneCaptureTriggers& triggers, bool effects)
    {
        return StartSceneResourceCapture(root / std::to_string(++attempt), 2, "fixture", "fixture", error, effects, 16 * 1024 * 1024, effects, false, 0, 0, triggers);
    };
    for (const char* bad : { "xyz", "1234567", "2cac681g", "2cac 681f", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0" })
    {
        UNSCOPED_INFO("a malformed pixel digest is refused");
        REQUIRE(!start({ bad, 0, 1 }, false));
    }
    UNSCOPED_INFO("windows outside 1..16 are refused");
    REQUIRE((!start({ "", 0, 0 }, false) && !start({ "", 0, 17 }, false)));
    UNSCOPED_INFO("route effects cannot span windows");
    REQUIRE(!start({ "", 0, 2 }, true));
    UNSCOPED_INFO("refusals leave no observation behind");
    REQUIRE(sceneCaptureSession.load() == nullptr);
    std::string normalized;
    UNSCOPED_INFO("digests normalize to lower case");
    REQUIRE((NormalizeScenePixelDigest("2CAC681F", normalized) && normalized == "2cac681f"));
    // SHA-256 of 03 02 01 00 07 06 05 04: the guest bytes with each 32-bit word swapped to host order.
    const std::array<std::uint8_t, 8> guest{ 0, 1, 2, 3, 4, 5, 6, 7 };
    UNSCOPED_INFO("the word digest swaps each guest word");
    REQUIRE(ScenePixelWordDigest(guest) == "957b6b0c2a372d64b414581f82ac8f6680445f1944592e39c299479c22a03e5b");
    UNSCOPED_INFO("a valid trigger starts");
    REQUIRE(start({ "2cac681f", 0, 1 }, false));
    StopSceneResourceCapture();
    return 0;
}

constexpr std::uint32_t           literalObject = 0x9000, literalTable = 0x9108;
constexpr std::uint64_t           literalPhysical = 0x1AEA0000;
std::array<std::uint8_t, 0x10000> literalVirtual{};
std::vector<std::uint8_t>         literalValues(0x20000);

std::span<const std::uint8_t> literalMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if (physical)
    {
        if (address >= literalPhysical && address + size <= literalPhysical + literalValues.size())
            return std::span(literalValues).subspan(address - literalPhysical, size);
    }
    else if (address == 0x82091394 || address == 0x820913A8)
        return std::span(literalVirtual).first(size);
    else if (address + size <= literalVirtual.size())
        return std::span(literalVirtual).subspan(address, size);
    throw std::runtime_error("scene_literal_fixture_extent");
}

void literalWord(std::size_t address, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        literalVirtual.at(address + i) = std::uint8_t(value >> (24 - 8 * i));
}

// PS-only literals honor recordBytes and supplied records.
int literalTest(std::string_view mode)
{
    // C10 header 941c0041...: literals 0xE0; metadata 0x108.
    constexpr std::uint8_t c10Header[68] = {
        0x00, 0x10, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFA, 0xE9, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x2A, 0x11, 0x00, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x00, 0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00, 0x01, 0x08
    };

    struct Value
    {
        std::string   role;
        std::uint32_t bytes, offset; // retained length and the record's offset from the resource base
    };

    struct Case
    {
        std::uint32_t              size;
        std::vector<std::uint32_t> records;
        std::vector<Value>         values;     // expected retains, in record order
        std::uint32_t              tableBytes; // expected retained table size; 0 when none
        std::string                error;      // expected payload_error
    };

    const std::uint32_t                            recordA = 0x01FC0010, recordB = 0x00200004, valuesLimit = 0x01FC4000;
    const std::map<std::string, Case, std::less<>> cases{
        { "literal-c10", { 20, { recordA, 0 }, { { "ps_literal_values:508", 64, 0 } }, 40, "" } },
        { "literal-two", { 24, { recordA, 0, recordB, 0x40 }, { { "ps_literal_values:508", 64, 0 }, { "ps_literal_values:32", 16, 0x40 } }, 44, "" } },
        { "literal-terminator", { 40, { recordA, 0, 0, 0, recordB, 0x40 }, { { "ps_literal_values:508", 64, 0 } }, 60, "" } },
        { "literal-straddle", { 12, { recordA, 0, recordB, 0x40 }, { { "ps_literal_values:508", 64, 0 }, { "ps_literal_values:32", 16, 0x40 } }, 36, "" } },
        { "literal-zero", { 0, {}, {}, 20, "" } },
        { "literal-limit", { 4096, { recordA, 0 }, { { "ps_literal_values:508", 64, 0 } }, 4116, "" } },
        { "literal-over", { 4100, { recordA, 0 }, {}, 0, "scene_literal_extent" } },
        { "literal-short", { 4, { recordA, 0 }, {}, 0, "scene_literal_extent" } },
        { "literal-values-limit", { 8, { valuesLimit, 0 }, { { "ps_literal_values:508", 65536, 0 } }, 28, "" } },
        { "literal-values-over", { 8, { valuesLimit + 1, 0 }, {}, 0, "scene_literal_values_extent" } },
    };
    const auto found = cases.find(mode);
    UNSCOPED_INFO("known literal case");
    REQUIRE(found != cases.end());
    const auto& c = found->second;
    SetSceneTestMemoryReader(literalMemory);
    for (std::size_t i = 0; i < literalValues.size(); ++i)
        literalValues[i] = std::uint8_t(i * 7 + 1);
    // State at 0x2000: the PS object at G+0x3194 and a zero-element declaration at G+0x2E2C.
    literalWord(0x2000 + 0x3194, literalObject);
    literalWord(0x2000 + 0x2E2C, 0x7000);
    std::copy(std::begin(c10Header), std::end(c10Header), literalVirtual.begin() + literalObject);
    // Only the movie's own table sits directly before its metadata; the others move the metadata clear of it.
    const bool c10 = mode == "literal-c10";
    if (!c10)
        literalWord(literalObject + 64, 0x2000);
    const std::uint32_t metadata = literalObject + 40 + (c10 ? 0x108 : 0x2000);
    literalWord(metadata, 0x18000);
    literalWord(metadata + 4, 144);
    literalWord(literalTable + 16, c.size);
    for (std::size_t i = 0; i < c.records.size(); ++i)
        literalWord(literalTable + 20 + i * 4, c.records[i]);

    const auto  directory = std::filesystem::temp_directory_path() /
                            ("rerevved-scene-literal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    UNSCOPED_INFO("start literal fixture");
    REQUIRE(StartSceneResourceCapture(directory, 2, "fixture", "fixture", error, true, 16 * 1024 * 1024));
    auto* opaque = sceneCaptureSession.load();
    std::ofstream(directory / "arm") << "fixture";
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        NotifySceneResourceFrameBoundary();
        if (SceneTestWindowActive(opaque))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UNSCOPED_INFO("literal fixture arms");
        REQUIRE(attempt != 99);
    }
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    ctx.r3.u32 = 0x2000;
    EndSceneEntry(BeginSceneEntry(opaque, 0x826A3000, ctx), ctx);
    NotifySceneResourceFrameBoundary();
    NotifySceneResourceFrameBoundary();
    await([&]
          {
              return std::filesystem::exists(directory / "result.toml");
          });
    StopSceneResourceCapture();

    const auto result = toml::parse_file((directory / "result.toml").string());
    UNSCOPED_INFO("a literal refusal leaves the metadata capture complete");
    REQUIRE((result["complete"].value_or(false) && result["relevant_read_failures"].value_or(-1) == 0));
    UNSCOPED_INFO("payload failure count");
    REQUIRE(result["payload_failures"].value_or(-1) == (c.error.empty() ? 0 : 1));
    std::string payloadError;
    for (const auto& item : *toml::parse_file((directory / "events.toml").string())["events"].as_array())
        if ((*item.as_table())["kind"].value_or(std::string_view{}) == "draw")
            payloadError = (*item.as_table())["payload_error"].value_or(std::string{});
    UNSCOPED_INFO("payload error");
    REQUIRE(payloadError == c.error);
    std::vector<std::tuple<std::string, std::uint32_t, std::string>> values;
    std::uint32_t                                                    tableBytes = 0;
    for (const auto& item : *toml::parse_file((directory / "payload-manifest.toml").string())["bindings"].as_array())
    {
        const auto&       row  = *item.as_table();
        const std::string role = std::string(row["role"].value_or(std::string_view{}));
        if (role.starts_with("ps_literal_values:"))
        {
            values.emplace_back(role, row["bytes"].value_or(0U), std::string(row["sha256"].value_or(std::string_view{})));
        }
        else if (role == "ps_literal_table")
            tableBytes = row["bytes"].value_or(0U);
    }
    UNSCOPED_INFO("retained literal value ranges");
    REQUIRE(values.size() == c.values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const auto& [role, bytes, sha] = values[i];
        UNSCOPED_INFO("retained literal value ranges");
        REQUIRE((role == c.values[i].role && bytes == c.values[i].bytes));
        UNSCOPED_INFO("values retain the guest bytes at resourceBase + offset");
        REQUIRE(sha == rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(literalValues.data()) + c.values[i].offset, bytes)));
    }
    UNSCOPED_INFO("retained literal table extent");
    REQUIRE(tableBytes == c.tableBytes);
    return 0;
}

// Ends any capture and the memory reader a case installed, however it ends.
struct SceneCaseGuard
{
    ~SceneCaseGuard()
    {
        StopAndResetSceneResourceCaptureForTests();
        SetSceneTestMemoryReader(nullptr);
    }
};

// Each case starts from the memory images and flags a fresh process had.
void resetSceneFixtures()
{
    guestState.fill(0);
    guestZero.fill(0);
    writerGuest.fill(0);
    writerPhysical.fill(0);
    failWriterDestination = failWriterRelease = false;
    payloadGraphics.fill(0);
    payloadFormats.fill(0);
    std::fill(payloadVertices.begin(), payloadVertices.end(), std::uint8_t{});
    std::fill(payloadCandidate.begin(), payloadCandidate.end(), std::uint8_t{});
    sampledReads = payloadReads = 0;
    triggerGuest.fill(0);
    triggerPhysical.fill(0);
    raceBlock = raceEntered = raceRelease = false;
    literalVirtual.fill(0);
    std::fill(literalValues.begin(), literalValues.end(), std::uint8_t{});
}

template <class Test>
void runScene(Test&& test)
{
    SceneCaseGuard guard;
    resetSceneFixtures();
    UNSCOPED_INFO("the recorder case reports failure");
    REQUIRE(test() == 0);
}

// The capture directory a helper run was given, or empty.
std::filesystem::path manualCapture()
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    return args.argc() >= 1 ? std::filesystem::path(std::string(args[1])) : std::filesystem::path{};
}

} // namespace

// One ctest case per old registration: scene_resource_<mode>.
#define SCENE_CASE(name, ...)                                                \
    TEST_CASE("scene_resource_" name, "[ctest][scene-resource][timeout-30]") \
    {                                                                        \
        runScene([]                                                          \
                 {                                                           \
                     return __VA_ARGS__;                                     \
                 });                                                         \
    }

SCENE_CASE("capture_failure", recorderTest(false))
SCENE_CASE("capture_shutdown", recorderTest(true))
SCENE_CASE("capture_writers", writerTest())
SCENE_CASE("capture_writer_read_failure", writerReadFailureTest())
SCENE_CASE("payload", payloadTest("payload", {}))
SCENE_CASE("payload-targets", payloadTest("payload-targets", {}))
SCENE_CASE("payload-regions", payloadTest("payload-regions", {}))
SCENE_CASE("payload-regions-copy", payloadTest("payload-regions-copy", {}))
SCENE_CASE("payload-region-failure", payloadTest("payload-region-failure", {}))
SCENE_CASE("payload-region-window", payloadTest("payload-region-window", {}))
SCENE_CASE("payload-reuse", payloadTest("payload-reuse", {}))
SCENE_CASE("payload-gaps", payloadTest("payload-gaps", {}))
SCENE_CASE("payload-coalescing", payloadTest("payload-coalescing", {}))
SCENE_CASE("payload-capacity", payloadTest("payload-capacity", {}))
SCENE_CASE("payload-allocator", payloadTest("payload-allocator", {}))
SCENE_CASE("payload-wide-stride", payloadTest("payload-wide-stride", {}))
SCENE_CASE("payload-io-failure", payloadTest("payload-io-failure", {}))
SCENE_CASE("legacy-schema", payloadTest("legacy-schema", {}))
SCENE_CASE("arm-frame", armTest("arm-frame"))
SCENE_CASE("arm-file-first", armTest("arm-file-first"))
SCENE_CASE("arm-frame-first", armTest("arm-frame-first"))
SCENE_CASE("arm-off", armTest("arm-off"))
SCENE_CASE("arm-digest", digestArmTest("arm-digest"))
SCENE_CASE("arm-digest-next", digestArmTest("arm-digest-next"))
SCENE_CASE("arm-digest-race", digestRaceTest())
SCENE_CASE("arm-digest-refused", digestRefusedTest())
SCENE_CASE("arm-windows", windowsTest())
SCENE_CASE("payload-windows", payloadWindowsTest({}))
SCENE_CASE("literal-c10", literalTest("literal-c10"))
SCENE_CASE("literal-two", literalTest("literal-two"))
SCENE_CASE("literal-terminator", literalTest("literal-terminator"))
SCENE_CASE("literal-straddle", literalTest("literal-straddle"))
SCENE_CASE("literal-zero", literalTest("literal-zero"))
SCENE_CASE("literal-limit", literalTest("literal-limit"))
SCENE_CASE("literal-over", literalTest("literal-over"))
SCENE_CASE("literal-short", literalTest("literal-short"))
SCENE_CASE("literal-values-limit", literalTest("literal-values-limit"))
SCENE_CASE("literal-values-over", literalTest("literal-values-over"))
#undef SCENE_CASE

// Recorder modes the analysis script runs as
//   rerevved_tests --helper scene-resource.<mode> <capture-directory>
#define SCENE_RECORDER_HELPER(identifier, mode)                             \
    TEST_CASE("scene_resource_manual_" mode, "[.][manual][scene-resource]") \
    {                                                                       \
        runScene([]                                                         \
                 {                                                          \
                     return payloadTest(mode, manualCapture());             \
                 });                                                        \
    }                                                                       \
    REREVVED_MANUAL_HELPER(identifier, "scene-resource." mode, "scene_resource_manual_" mode)

SCENE_RECORDER_HELPER(sceneRecorderLegacySchema, "legacy-schema")
SCENE_RECORDER_HELPER(sceneRecorderPayload, "payload")
SCENE_RECORDER_HELPER(sceneRecorderPayloadGaps, "payload-gaps")
SCENE_RECORDER_HELPER(sceneRecorderPayloadCoalescing, "payload-coalescing")
SCENE_RECORDER_HELPER(sceneRecorderPayloadAllocator, "payload-allocator")
SCENE_RECORDER_HELPER(sceneRecorderPayloadRegions, "payload-regions")
SCENE_RECORDER_HELPER(sceneRecorderPayloadRegionsCopy, "payload-regions-copy")
SCENE_RECORDER_HELPER(sceneRecorderPayloadRegionFailure, "payload-region-failure")
SCENE_RECORDER_HELPER(sceneRecorderPayloadRegionWindow, "payload-region-window")
SCENE_RECORDER_HELPER(sceneRecorderPayloadPreArm, "payload-pre-arm")
SCENE_RECORDER_HELPER(sceneRecorderPayloadPreArmOverflow, "payload-pre-arm-overflow")
SCENE_RECORDER_HELPER(sceneRecorderPayloadReuse, "payload-reuse")
SCENE_RECORDER_HELPER(sceneRecorderPayloadOverlap, "payload-overlap")
SCENE_RECORDER_HELPER(sceneRecorderPayloadTargets, "payload-targets")
SCENE_RECORDER_HELPER(sceneRecorderPayloadIoFailure, "payload-io-failure")
#undef SCENE_RECORDER_HELPER

TEST_CASE("scene_resource_manual_payload-windows", "[.][manual][scene-resource]")
{
    runScene([]
             {
                 return payloadWindowsTest(manualCapture());
             });
}

REREVVED_MANUAL_HELPER(sceneRecorderPayloadWindows, "scene-resource.payload-windows", "scene_resource_manual_payload-windows")

TEST_CASE("scene_resource", "[ctest][scene-resource]")
{
    SceneTextureStorage storage;
    UNSCOPED_INFO("track padded base and mip storage");
    REQUIRE(storage.Set(1, { { 0x1000, 0x1000 }, { 0x4000, 0x1000 } }));
    UNSCOPED_INFO("copy inside base or mip is admitted");
    REQUIRE((storage.Overlaps(0xA0001000, 1) && storage.Overlaps(0xC0004FFF, 1)));
    UNSCOPED_INFO("straddling either edge is admitted");
    REQUIRE((storage.Overlaps(0xA0000FFF, 2) && storage.Overlaps(0xA0001FFF, 2)));
    UNSCOPED_INFO("E alias includes the SDK physical 0x1000 offset");
    REQUIRE(storage.Overlaps(0xE0000000, 1));
    UNSCOPED_INFO("outside, touching and unrelated low virtual storage are filtered");
    REQUIRE((!storage.Overlaps(0xA0002000, 1) && !storage.Overlaps(0xA0000FFF, 1) && !storage.Overlaps(0x1000, 1)));
    UNSCOPED_INFO("empty and wrapping copy ranges are filtered");
    REQUIRE((!storage.Overlaps(0xA0001000, 0) && !storage.Overlaps(0xFFFFFFFF, 2)));
    UNSCOPED_INFO("overlapping allocation tracked independently");
    REQUIRE(storage.Set(2, { { 0x1800, 0x1000 } }));
    storage.Release(1);
    UNSCOPED_INFO("release retires only its own allocation");
    REQUIRE((!storage.Overlaps(0xA0001000, 1) && storage.Overlaps(0xA0001800, 1)));
    storage.Release(2);
    UNSCOPED_INFO("copy after release is filtered");
    REQUIRE(!storage.Overlaps(0xA0001800, 1));
    UNSCOPED_INFO("two objects may reuse the same storage");
    REQUIRE((storage.Set(1, { { 0x1000, 0x1000 } }) && storage.Set(2, { { 0x1000, 0x1000 } })));
    UNSCOPED_INFO("empty replacement removes only its object's storage");
    REQUIRE((storage.Set(1, {}) && storage.Overlaps(0xA0001000, 1)));
    storage.Release(2);
    UNSCOPED_INFO("reused storage retires after the last object release");
    REQUIRE(!storage.Overlaps(0xA0001000, 1));
    for (std::uint32_t object = 1; object <= 8193; ++object)
    {
        UNSCOPED_INFO("non-texture creations do not consume object slots");
        REQUIRE(storage.Set(object, {}));
    }
    UNSCOPED_INFO("empty creations leave capacity for textures");
    REQUIRE(storage.Set(8194, { { 0x1000, 0x1000 } }));
    // tests/gpu/native_prepared_bc1_64_fixture.h:26, selected captured bytes.
    const SceneFetch captured{ 0x01000002, 0x1BC73052, 0x0007E03F, 0x00A80D10, 0x00000183, 0x1BC6FA18 };
    const auto       bc1 = DecodeSceneFetch(captured);
    UNSCOPED_INFO("captured BC1 packed-mip descriptor fixture");
    REQUIRE((bc1.width == 64 && bc1.height == 64 && bc1.format == 18 && bc1.maxMip == 6 &&
             bc1.base == 0x1BC73000 && bc1.mipBase == 0x1BC6F000 && bc1.endian == 1));
    const auto capturedResolve = DecodeSceneResolve(0, captured);
    UNSCOPED_INFO("resolve argument decoder on captured descriptor bytes");
    REQUIRE((capturedResolve.width == 64 && capturedResolve.height == 64 && capturedResolve.format == 18));
    // BE fixture packed from the documented xe_gpu_texture_fetch_t bit offsets:
    // tiled RGBA8 1280x720, pitch 1280, endian 2, identity swizzle, LOD 0..10.
    // SDK xenos.h:1168..1268; G+480 mapping sub_826ACEA0 at 826ACEB4/826ACFA4.
    const std::array<std::uint8_t, 24> bytes{
        0x8A, 0, 0, 0x56, 0x01, 0x23, 0x40, 0x86, 0, 0x59, 0xE4, 0xFF, 0, 0, 0x0D, 0x10, 0, 0, 2, 0x80, 0x02, 0x34, 0x5A, 0
    };
    SceneFetch w{};
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < 4; ++j)
            w[i] = (w[i] << 8) | bytes[i * 4 + j];
    const auto t = DecodeSceneFetch(w);
    UNSCOPED_INFO("fetch type/pitch/tiling decode");
    REQUIRE((t.type == 2 && t.tiled == 1 && t.pitch == 1280));
    UNSCOPED_INFO("fetch address/format/endian decode");
    REQUIRE((t.base == 0x1234000 && t.mipBase == 0x2345000 && t.format == 6 && t.endian == 2));
    UNSCOPED_INFO("fetch dimensions/mips decode");
    REQUIRE((t.width == 1280 && t.height == 720 && t.maxMip == 10 && t.packedMips == 1));
    UNSCOPED_INFO("fetch signs/swizzle/dimension decode");
    REQUIRE((t.signs == 0x15 && t.swizzle == 0x688 && t.dimension == 1));
    w[2] |= 5U << 26;
    UNSCOPED_INFO("non-stacked 2D ignores stack_depth bits (SDK depth-minus-one zero)");
    REQUIRE(DecodeSceneFetch(w).depth == 1);
    w[1] |= 1U << 10;
    UNSCOPED_INFO("stacked 2D retains array depth");
    REQUIRE(DecodeSceneFetch(w).depth == 6);
    w[1] &= ~(1U << 10);
    // RVA-F-0126: primary page and extent fields; generated sub_8269E520
    // 8269E5CC..8269E648 adds ((word3 << 2) & 2) + 1, not always one.
    const auto r = DecodeSceneResolve(0x40004, w);
    UNSCOPED_INFO("resolve source/base/format decode");
    REQUIRE((r.sourceIndex == 4 && r.primaryBase == t.base && r.format == 6));
    UNSCOPED_INFO("resolve two-dimensional extent decode");
    REQUIRE((r.width == 1280 && r.height == 720 && r.layoutIncrement == 1));
    UNSCOPED_INFO("resolve endian uses descriptor word1 bits6..7, not sign_x");
    REQUIRE(r.rawEndian == 2);
    w[3] |= 0x80000000;
    const auto increment = DecodeSceneResolve(0, w);
    UNSCOPED_INFO("resolve layout-dependent extent decode");
    REQUIRE((increment.width == 1282 && increment.height == 722 && increment.layoutIncrement == 3));
    w[5]             = 0x400;
    w[2]             = 31 | (63 << 11);
    const auto three = DecodeSceneResolve(7, w);
    UNSCOPED_INFO("resolve three-dimensional packed extent decode");
    REQUIRE((three.width == 34 && three.height == 66));
    SceneLog log(2, 4096);
    for (unsigned i = 0; i < 3; ++i)
    {
        auto p      = std::make_unique<ScenePacket>();
        p->sequence = i;
        log.Push(std::move(p));
    }
    UNSCOPED_INFO("queue capacity reports explicit overflow");
    REQUIRE(log.Overflow() == 1);
    UNSCOPED_INFO("queue preserves publication order");
    REQUIRE((log.Pop()->sequence == 0 && log.Pop()->sequence == 1 && !log.Pop()));
    SceneLog byteLog(4, sizeof(ScenePacket));
    auto     tooLarge = std::make_unique<ScenePacket>();
    tooLarge->state.resize(1);
    UNSCOPED_INFO("queue byte bound reports overflow");
    REQUIRE((!byteLog.Push(std::move(tooLarge)) && byteLog.Overflow() == 1));
    SceneLog     concurrent(1000, 1024 * 1024);
    std::jthread first([&]
                       {
                           for (unsigned i = 0; i < 500; ++i)
                           {
                               auto p      = std::make_unique<ScenePacket>();
                               p->sequence = i;
                               concurrent.Push(std::move(p));
                           }
                       });
    std::jthread second([&]
                        {
                            for (unsigned i = 0; i < 500; ++i)
                            {
                                auto p      = std::make_unique<ScenePacket>();
                                p->sequence = 500 + i;
                                concurrent.Push(std::move(p));
                            }
                        });
    first.join();
    second.join();
    std::array<bool, 1000> seen{};
    unsigned               count = 0;
    while (auto p = concurrent.Pop())
    {
        UNSCOPED_INFO("concurrent publication duplicates sequence");
        REQUIRE(!seen[p->sequence]);
        seen[p->sequence] = true;
        ++count;
    }
    UNSCOPED_INFO("concurrent writers retain every admitted event");
    REQUIRE((count == 1000 && !concurrent.Overflow()));

    // Constructed from SDK ucode.h layouts: exec_end at address 1 runs a
    // texture fetch of slot 5, a set-texture-LOD naming slot 7, then ALU.
    const std::array<std::uint32_t, 12> program{
        1 | 3 << 12 | 0b000101 << 16, 0x2000, 0, 1 | 5 << 20, 0, 0, 24 | 7 << 20, 0, 0, 0, 0, 0
    };
    std::vector<std::uint8_t> microcode;
    for (auto v : program)
        for (int shift = 24; shift >= 0; shift -= 8)
            microcode.push_back(std::uint8_t(v >> shift));
    UNSCOPED_INFO("texture fetch slot mask skips set-texture opcodes");
    REQUIRE(SceneTextureFetchMask(microcode) == 1U << 5);
    // Exec/cond_exec/cond_exec_pred word0 fields are identical in ucode.h.
    // The opcode is word1 bits12..15 in the first half of the CF pair.
    for (auto opcode : { 1U, 2U, 3U, 4U, 5U, 6U, 13U, 14U })
    {
        microcode[6] = std::uint8_t(opcode << 4);
        UNSCOPED_INFO("conditional exec views retain BE address/count/sequence");
        REQUIRE(SceneTextureFetchMask(microcode) == 1U << 5);
    }
    for (auto opcode : { 24U, 25U, 26U })
    {
        microcode[27] = std::uint8_t(opcode);
        UNSCOPED_INFO("all three set-texture opcodes skip bindings");
        REQUIRE(SceneTextureFetchMask(microcode) == 1U << 5);
    }
    bool threw = false;
    try
    {
        SceneTextureFetchMask(std::span(microcode).first(24));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    UNSCOPED_INFO("exec clause past the code is rejected");
    REQUIRE(threw);
    threw = false;
    try
    {
        SceneTextureFetchMask(std::span(microcode).first(47));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    UNSCOPED_INFO("partial BE shader words are rejected");
    REQUIRE(threw);
}
