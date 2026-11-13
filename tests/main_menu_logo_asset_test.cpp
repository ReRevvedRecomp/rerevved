#include "main_menu_logo_asset.h"
#include "registry_scope.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{

void writeLittleEndianU32(std::vector<uint8_t>& bytes,
                          size_t                offset,
                          uint32_t              value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> makeLogoDds(uint8_t marker)
{
    std::vector<uint8_t> bytes(rerevved::main_menu_logo::kLogoDdsSize, 0);
    std::memcpy(bytes.data(), "DDS ", 4);
    writeLittleEndianU32(bytes, 4, 124);
    writeLittleEndianU32(bytes, 8, 0x00001007);
    writeLittleEndianU32(bytes, 12, 256);
    writeLittleEndianU32(bytes, 16, 1024);
    writeLittleEndianU32(bytes, 20, 4096);
    writeLittleEndianU32(bytes, 28, 0);
    writeLittleEndianU32(bytes, 76, 32);
    writeLittleEndianU32(bytes, 80, 0x41);
    writeLittleEndianU32(bytes, 84, 0);
    writeLittleEndianU32(bytes, 88, 32);
    writeLittleEndianU32(bytes, 92, 0x00ff0000);
    writeLittleEndianU32(bytes, 96, 0x0000ff00);
    writeLittleEndianU32(bytes, 100, 0x000000ff);
    writeLittleEndianU32(bytes, 104, 0xff000000);
    writeLittleEndianU32(bytes, 108, 0x1000);
    bytes[128] = marker;
    return bytes;
}

struct ResolverState
{
    rex::Result<rex::system::AssetOverlayResolution> result =
        rex::Err<rex::system::AssetOverlayResolution>(
            rex::ErrorCategory::NotFound, "missing");
    std::string key;
    size_t      maxBytes     = 0;
    size_t      packageCount = 0;
};

ResolverState resolverState;

rex::Result<rex::system::AssetOverlayResolution> fakeResolver(
    std::span<const rex::system::AssetOverlayPackage> packages,
    std::string_view                                  key,
    size_t                                            maxBytes)
{
    resolverState.key          = std::string(key);
    resolverState.maxBytes     = maxBytes;
    resolverState.packageCount = packages.size();
    return resolverState.result;
}

void TestValidator()
{
    auto data = makeLogoDds(0x11);
    UNSCOPED_INFO("valid legacy logo DDS rejected");
    REQUIRE(rerevved::main_menu_logo::IsValidLogoDds(data));

    data[4] = 0;
    UNSCOPED_INFO("invalid DDS magic accepted");
    REQUIRE(!rerevved::main_menu_logo::IsValidLogoDds(data));
    data[4] = 124;

    writeLittleEndianU32(data, 28, 1);
    UNSCOPED_INFO("mipmapped DDS accepted");
    REQUIRE(!rerevved::main_menu_logo::IsValidLogoDds(data));
    writeLittleEndianU32(data, 28, 0);

    writeLittleEndianU32(data, 84, 0x30315844);
    UNSCOPED_INFO("DX10 DDS accepted");
    REQUIRE(!rerevved::main_menu_logo::IsValidLogoDds(data));
}

void TestMissingFallsBack()
{
    rerevved::main_menu_logo::ResetForTests();
    resolverState.result = rex::Err<rex::system::AssetOverlayResolution>(
        rex::ErrorCategory::NotFound, "missing");

    std::vector<rex::system::AssetOverlayPackage> packages(1);
    UNSCOPED_INFO("missing asset selected");
    REQUIRE(!rerevved::main_menu_logo::ResolveSelectedLogo(
        packages, &fakeResolver));
    rerevved::main_menu_logo::Payload payload;
    UNSCOPED_INFO("missing asset retained a payload");
    REQUIRE(!rerevved::main_menu_logo::TryGetPayload(payload));
    UNSCOPED_INFO("resolver adapter arguments changed");
    REQUIRE((resolverState.key == rerevved::main_menu_logo::kAssetKey &&
             resolverState.maxBytes == rerevved::main_menu_logo::kLogoDdsSize &&
             resolverState.packageCount == 1));
}

void TestMalformedWinnerDoesNotFallThrough()
{
    rerevved::main_menu_logo::ResetForTests();
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id = "top.mod";
    resolved.bytes      = makeLogoDds(0x21);
    resolved.bytes.resize(128);
    resolved.shadowed_package_ids = { "lower.mod" };
    resolverState.result          = std::move(resolved);

    std::vector<rex::system::AssetOverlayPackage> packages(2);
    UNSCOPED_INFO("malformed winner accepted");
    REQUIRE(!rerevved::main_menu_logo::ResolveSelectedLogo(
        packages, &fakeResolver));
    rerevved::main_menu_logo::Payload payload;
    UNSCOPED_INFO("malformed winner retained a payload");
    REQUIRE(!rerevved::main_menu_logo::TryGetPayload(payload));
}

void TestValidWinnerIsRetained()
{
    rerevved::main_menu_logo::ResetForTests();
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id           = "top.mod";
    resolved.bytes                = makeLogoDds(0x42);
    resolved.shadowed_package_ids = { "lower.mod", "retail" };
    resolverState.result          = std::move(resolved);

    std::vector<rex::system::AssetOverlayPackage> packages(3);
    UNSCOPED_INFO("valid winner was rejected");
    REQUIRE(rerevved::main_menu_logo::ResolveSelectedLogo(
        packages, &fakeResolver));

    rerevved::main_menu_logo::Payload payload;
    UNSCOPED_INFO("valid winner payload was not retained");
    REQUIRE((rerevved::main_menu_logo::TryGetPayload(payload) &&
             payload && payload->at(128) == 0x42));
}

void TestScaleShrinksAboutTheCenter()
{
    namespace logo = rerevved::main_menu_logo;
    auto source    = makeLogoDds(0);
    for (size_t offset = 128; offset < source.size(); offset += 4)
    {
        source[offset + 0] = 10;
        source[offset + 1] = 20;
        source[offset + 2] = 30;
        source[offset + 3] = 255;
    }
    UNSCOPED_INFO("a scale of 1 changed the logo");
    REQUIRE(logo::ScaleLogo(source, 1.0f) == source);

    const auto scaled = logo::ScaleLogo(source, 0.5f);
    const auto pixel  = [&scaled](size_t x, size_t y)
    {
        return scaled.data() + 128 + (y * 1024 + x) * 4;
    };
    UNSCOPED_INFO("the header or size changed");
    REQUIRE((scaled.size() == source.size() && std::memcmp(scaled.data(), source.data(), 128) == 0));
    UNSCOPED_INFO("the center lost its texel");
    REQUIRE((pixel(512, 128)[0] == 10 && pixel(512, 128)[1] == 20 && pixel(512, 128)[2] == 30 && pixel(512, 128)[3] == 255));
    UNSCOPED_INFO("the image still covers the border");
    REQUIRE((pixel(0, 0)[3] == 0 && pixel(1023, 255)[3] == 0 && pixel(200, 128)[3] == 0 && pixel(512, 40)[3] == 0));
    UNSCOPED_INFO("the half-size image does not span the middle half");
    REQUIRE((pixel(300, 100)[3] == 255 && pixel(700, 160)[3] == 255));
}

void TestScaledWinnerIsRetained()
{
    rerevved::main_menu_logo::ResetForTests();
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id  = "top.mod";
    resolved.bytes       = makeLogoDds(0);
    resolved.bytes[131]  = 255;
    resolverState.result = std::move(resolved);

    std::vector<rex::system::AssetOverlayPackage> packages(1);
    REQUIRE(rerevved::main_menu_logo::ResolveSelectedLogo(packages, &fakeResolver, rerevved::main_menu_logo::kNarrowScale));
    rerevved::main_menu_logo::Payload payload;
    UNSCOPED_INFO("the corner texel survived the shrink");
    REQUIRE((rerevved::main_menu_logo::TryGetPayload(payload) && payload && payload->at(131) == 0));
}

} // namespace

TEST_CASE("main_menu_logo_asset", "[ctest][main-menu-logo-asset]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestValidator();
    TestMissingFallsBack();
    TestMalformedWinnerDoesNotFallThrough();
    TestValidWinnerIsRetained();
    TestScaleShrinksAboutTheCenter();
    TestScaledWinnerIsRetained();
}
