#include "save_sidecar.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <random>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{

namespace sidecar   = rerevved::save_sidecar;
namespace overrides = rerevved::resource_identity_overrides;

const sidecar::State    kSaved{ true, true, 33, 1, { { 0, 0 }, { 402, 9 }, { 1271, 18 } } };
const sidecar::Identity kIdentity{ "save0.sve", 7, std::string(64, 'a') };

void testPayload()
{
    const auto bytes   = sidecar::Serialize(kSaved, kIdentity);
    const auto decoded = sidecar::Deserialize(bytes, kIdentity);
    UNSCOPED_INFO("frozen values and canonical Balanced records round trip");
    CHECK((decoded && *decoded == kSaved));
    for (int32_t mode = 0; mode <= 3; ++mode)
    {
        const sidecar::State empty{ false, false, 33, mode, {} };
        UNSCOPED_INFO("empty overrides round trip for every Resources value");
        CHECK(sidecar::Deserialize(sidecar::Serialize(empty, kIdentity), kIdentity) == empty);
    }
    auto identity = kIdentity;
    identity.slot = "save1.sve";
    UNSCOPED_INFO("another slot rejects the payload");
    CHECK(!sidecar::Deserialize(bytes, identity));
    identity        = kIdentity;
    identity.device = 8;
    UNSCOPED_INFO("another device rejects the payload");
    CHECK(!sidecar::Deserialize(bytes, identity));
    identity        = kIdentity;
    identity.digest = std::string(64, 'b');
    UNSCOPED_INFO("another native content digest rejects the payload");
    CHECK(!sidecar::Deserialize(bytes, identity));
    for (size_t length = 0; length < bytes.size(); ++length)
    {
        UNSCOPED_INFO("every truncated payload is rejected");
        CHECK(!sidecar::Deserialize(std::span(bytes).first(length), kIdentity));
    }
    auto invalid = bytes;
    invalid.push_back(0);
    UNSCOPED_INFO("trailing bytes are rejected");
    CHECK(!sidecar::Deserialize(invalid, kIdentity));
    invalid    = bytes;
    invalid[4] = sidecar::kSchemaVersion + 1;
    UNSCOPED_INFO("unknown schema is rejected");
    CHECK(!sidecar::Deserialize(invalid, kIdentity));
    const size_t stateOffset = 6 + kIdentity.slot.size() + 4 + 64;
    for (size_t field : { size_t{ 0 }, size_t{ 1 } })
    {
        invalid                      = bytes;
        invalid[stateOffset + field] = 2;
        UNSCOPED_INFO("non-boolean policy fields are rejected");
        CHECK(!sidecar::Deserialize(invalid, kIdentity));
    }
    for (uint8_t population : { uint8_t{ 31 }, uint8_t{ 128 } })
    {
        invalid                  = bytes;
        invalid[stateOffset + 2] = population;
        UNSCOPED_INFO("OCC policy respects the supported signed-byte range");
        CHECK(!sidecar::Deserialize(invalid, kIdentity));
    }
    invalid                  = bytes;
    invalid[stateOffset + 3] = 0;
    UNSCOPED_INFO("Normal cannot carry overrides");
    CHECK(!sidecar::Deserialize(invalid, kIdentity));
    auto state = kSaved;
    std::swap(state.overrides[0], state.overrides[1]);
    UNSCOPED_INFO("unsorted records are rejected by the existing serializer");
    CHECK(sidecar::Serialize(state, kIdentity).empty());
    state.overrides = { { 0, 0 }, { 0, 9 } };
    UNSCOPED_INFO("duplicate tiles are rejected by the existing serializer");
    CHECK(sidecar::Serialize(state, kIdentity).empty());
    invalid                  = bytes;
    invalid[stateOffset + 9] = 22;
    UNSCOPED_INFO("unsupported override ordinals are rejected by the existing deserializer");
    CHECK(!sidecar::Deserialize(invalid, kIdentity));
}

void testLegendaryPayload()
{
    // Five players, eight records each, including every Legendary ordinal.
    constexpr std::array<uint8_t, 11>  ordinals  = { 18, 0, 19, 20, 8, 13, 9, 1, 12, 15, 21 };
    constexpr std::array<int8_t, 11>   terrains  = { 0, 1, 1, 1, 2, 2, 3, 4, 5, 6, 6 };
    constexpr std::array<uint64_t, 11> positions = { 2, 0, 2, 1, 0, 1, 1, 0, 2, 1, 2 };
    sidecar::State                     saved{ true, true, 33, 3, {} };
    for (uint16_t i = 0; i < 40; ++i)
    {
        saved.overrides.push_back({ static_cast<uint16_t>(40 * (i / 8) + i % 8), ordinals[i % ordinals.size()] });
    }
    const auto bytes   = sidecar::Serialize(saved, kIdentity);
    const auto decoded = sidecar::Deserialize(bytes, kIdentity);
    UNSCOPED_INFO("Legendary mode and forty records round trip through the sidecar without a schema change");
    CHECK(decoded == saved);
    sidecar::PendingLoad pending;
    pending.Stage(decoded);
    UNSCOPED_INFO("prior Balanced snapshot publishes");
    CHECK(overrides::Publish(kSaved.overrides));
    overrides::Clear();
    const auto& restored = pending.AfterReset();
    UNSCOPED_INFO("Legendary records restore after load reset over live Normal");
    CHECK((restored && restored->resources == 3 && overrides::Publish(restored->overrides)));
    for (size_t i = 0; i < saved.overrides.size(); ++i)
    {
        const auto pool   = i % ordinals.size();
        const auto record = saved.overrides[i];
        const auto sum    = overrides::CoordinateSum(*overrides::Current(), record.tileIndex, terrains[pool], 62);
        UNSCOPED_INFO("reload preserves each Legendary resource identity");
        CHECK((sum == positions[pool] && overrides::kIdentities[terrains[pool]][sum] == record.ordinal));
    }
    for (int32_t mode : { 0, 2 })
    {
        saved.resources = mode;
        UNSCOPED_INFO("inactive Resources modes still reject nonempty snapshots");
        CHECK(sidecar::Serialize(saved, kIdentity).empty());
    }
    overrides::Clear();
}

void testStandardRulesPayload()
{
    const sidecar::State standard{ false, false, 33, 0, {} };
    sidecar::PendingLoad pending;
    pending.Stage(sidecar::Deserialize(sidecar::Serialize(standard, kIdentity), kIdentity));
    auto effective = kSaved;
    UNSCOPED_INFO("nonstandard prior overrides publish");
    CHECK(overrides::Publish(kSaved.overrides));
    overrides::Clear();
    if (const auto& restored = pending.AfterReset())
    {
        effective = *restored;
        UNSCOPED_INFO("standard save restores empty overrides");
        CHECK(overrides::Publish(restored->overrides));
    }
    UNSCOPED_INFO("saved standard values win over nonstandard live selections");
    CHECK((effective == standard && overrides::Current()->empty()));
}

void testRestoreOrderingAndFallback()
{
    sidecar::PendingLoad pending;
    pending.Stage(sidecar::Deserialize(sidecar::Serialize(kSaved, kIdentity), kIdentity));
    sidecar::State live{ false, false, 33, 0, {} };
    UNSCOPED_INFO("prior game's overrides publish");
    CHECK(overrides::Publish(kSaved.overrides));
    // Model the native load ingress: reset and live freezes precede publication.
    overrides::Clear();
    auto effective = live;
    if (auto restored = pending.AfterReset())
    {
        effective = *restored;
        UNSCOPED_INFO("restored snapshot publishes after the reset");
        CHECK(overrides::Publish(restored->overrides));
    }
    UNSCOPED_INFO("saved settings and records survive load reset despite opposite live settings");
    CHECK((effective == kSaved && *overrides::Current() == kSaved.overrides));
    overrides::Clear();
    effective = live;
    if (const auto& restored = pending.AfterReset())
    {
        effective = *restored;
        UNSCOPED_INFO("second reset republishes the staged snapshot");
        CHECK(overrides::Publish(restored->overrides));
    }
    UNSCOPED_INFO("repeated reset in the same transaction restores the same saved state");
    CHECK((effective == kSaved && *overrides::Current() == kSaved.overrides));
    UNSCOPED_INFO("a new transaction inherits no staged state");
    CHECK(!sidecar::PendingLoad{}.AfterReset());

    for (const auto& missing : { std::optional<sidecar::State>{}, sidecar::Deserialize({}, kIdentity) })
    {
        pending.Stage(kSaved);
        pending.Stage(missing);
        overrides::Clear();
        effective = live;
        if (auto restored = pending.AfterReset())
        {
            effective = *restored;
            overrides::Publish(restored->overrides);
        }
        UNSCOPED_INFO("missing or invalid sidecar keeps live freezes and empty overrides");
        CHECK((effective == live && overrides::Current()->empty()));
    }
}

void writeFile(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    UNSCOPED_INFO("test file is written");
    CHECK(static_cast<bool>(file));
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void testFiles()
{
    const auto root = std::filesystem::temp_directory_path() / ("rerevved-sidecar-test-" + std::to_string(std::random_device{}()));
    if (!std::filesystem::create_directory(root))
    {
        UNSCOPED_INFO("test gets an exclusive temporary directory");
        CHECK(false);
        return;
    }
    const auto native    = root / "save0.sve";
    const auto path      = sidecar::SidecarPath(native);
    auto       temporary = path;
    temporary += ".tmp";
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    auto collision = path;
    collision += ".tmp." + std::to_string(process) + ".0";
    sidecar::ReadFailure failure;
    UNSCOPED_INFO("sidecar stays beside its namespaced native slot");
    CHECK((path.parent_path() == native.parent_path() && path.filename() == "save0.sve.rerevved-gameplay"));
    UNSCOPED_INFO("failed native save does not create files");
    CHECK(!sidecar::Save(false, native, "save0.sve", 7, kSaved));
    UNSCOPED_INFO("save failure writes neither destination nor temporary");
    CHECK((!std::filesystem::exists(path) && !std::filesystem::exists(temporary)));
    writeFile(native, "native content generation one");
    UNSCOPED_INFO("pre-sidecar save reports a missing sidecar");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::Missing));
    writeFile(temporary, "leftover legacy temporary");
    writeFile(collision, "leftover exclusive temporary");
    UNSCOPED_INFO("successful native save commits the sidecar");
    CHECK(sidecar::Save(true, native, "save0.sve", 7, kSaved));
    UNSCOPED_INFO("leftover temporaries neither block a save nor lose their bytes");
    CHECK((readFile(temporary) == "leftover legacy temporary" && readFile(collision) == "leftover exclusive temporary"));
    std::filesystem::remove(temporary);
    std::filesystem::remove(collision);
    UNSCOPED_INFO("disk sidecar restores its settings without a failure reason");
    CHECK((sidecar::Read(native, "save0.sve", 7, &failure) == kSaved && failure == sidecar::ReadFailure::None));
    const auto original = readFile(path);
    UNSCOPED_INFO("failed overwrite is rejected");
    CHECK(!sidecar::Save(false, native, "save0.sve", 7, {}));
    UNSCOPED_INFO("failed save preserves an existing sidecar without a temporary write");
    CHECK((readFile(path) == original && !std::filesystem::exists(temporary)));
    UNSCOPED_INFO("sidecar save leaves native bytes unchanged");
    CHECK(readFile(native) == "native content generation one");
    UNSCOPED_INFO("disk reads report slot identity rejection");
    CHECK((!sidecar::Read(native, "save1.sve", 7, &failure) && failure == sidecar::ReadFailure::Rejected));
    UNSCOPED_INFO("disk reads report device identity rejection");
    CHECK((!sidecar::Read(native, "save0.sve", 8, &failure) && failure == sidecar::ReadFailure::Rejected));
    writeFile(native, "native content generation two");
    UNSCOPED_INFO("overwritten slot reports a native digest mismatch");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::DigestMismatch));
    const sidecar::State replacement{ false, false, 33, 3, { { 0, 20 }, { 401, 21 } } };
    UNSCOPED_INFO("successful overwrite atomically replaces the old sidecar");
    CHECK(sidecar::Save(true, native, "save0.sve", 7, replacement));
    UNSCOPED_INFO("replacement restores and leaves no temporary");
    CHECK((sidecar::Read(native, "save0.sve", 7) == replacement && !std::filesystem::exists(temporary)));
    std::filesystem::remove(native);
    UNSCOPED_INFO("missing native file reports an unreadable digest");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::DigestUnreadable));
    auto invalid = readFile(path);
    invalid[4]   = sidecar::kSchemaVersion + 1;
    writeFile(path, invalid);
    UNSCOPED_INFO("schema rejection precedes native digest access");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::Rejected));
    writeFile(path, "invalid sidecar");
    UNSCOPED_INFO("malformed payload rejects before native digest access");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::Rejected));
    std::filesystem::remove(path);
    UNSCOPED_INFO("missing sidecar is detected before native digest access");
    CHECK((!sidecar::Read(native, "save0.sve", 7, &failure) && failure == sidecar::ReadFailure::Missing));
    std::filesystem::remove(root);
}

} // namespace

TEST_CASE("save_sidecar", "[ctest][save-sidecar][local-fast]")
{
    testPayload();
    testRestoreOrderingAndFallback();
    testStandardRulesPayload();
    testLegendaryPayload();
    testFiles();
}
