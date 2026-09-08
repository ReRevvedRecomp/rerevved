#include "main_menu_logo_asset.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

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
    require(rerevved::main_menu_logo::IsValidLogoDds(data),
            "valid legacy logo DDS rejected");

    data[4] = 0;
    require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "invalid DDS magic accepted");
    data[4] = 124;

    writeLittleEndianU32(data, 28, 1);
    require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "mipmapped DDS accepted");
    writeLittleEndianU32(data, 28, 0);

    writeLittleEndianU32(data, 84, 0x30315844);
    require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "DX10 DDS accepted");
}

void TestMissingFallsBack()
{
    rerevved::main_menu_logo::ResetForTests();
    resolverState.result = rex::Err<rex::system::AssetOverlayResolution>(
        rex::ErrorCategory::NotFound, "missing");

    std::vector<rex::system::AssetOverlayPackage> packages(1);
    require(!rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &fakeResolver),
            "missing asset selected");
    rerevved::main_menu_logo::Payload payload;
    require(!rerevved::main_menu_logo::TryGetPayload(payload),
            "missing asset retained a payload");
    require(resolverState.key == rerevved::main_menu_logo::kAssetKey &&
                resolverState.maxBytes == rerevved::main_menu_logo::kLogoDdsSize &&
                resolverState.packageCount == 1,
            "resolver adapter arguments changed");
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
    require(!rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &fakeResolver),
            "malformed winner accepted");
    rerevved::main_menu_logo::Payload payload;
    require(!rerevved::main_menu_logo::TryGetPayload(payload),
            "malformed winner retained a payload");
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
    require(rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &fakeResolver),
            "valid winner was rejected");

    rerevved::main_menu_logo::Payload payload;
    require(rerevved::main_menu_logo::TryGetPayload(payload) &&
                payload && payload->at(128) == 0x42,
            "valid winner payload was not retained");
}

} // namespace

int main()
{
    TestValidator();
    TestMissingFallsBack();
    TestMalformedWinnerDoesNotFallThrough();
    TestValidWinnerIsRetained();
    return 0;
}
