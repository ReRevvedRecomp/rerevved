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

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void WriteLittleEndianU32(std::vector<uint8_t>& bytes,
                          size_t                offset,
                          uint32_t              value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> MakeLogoDds(uint8_t marker)
{
    std::vector<uint8_t> bytes(rerevved::main_menu_logo::kLogoDdsSize, 0);
    std::memcpy(bytes.data(), "DDS ", 4);
    WriteLittleEndianU32(bytes, 4, 124);
    WriteLittleEndianU32(bytes, 8, 0x00001007);
    WriteLittleEndianU32(bytes, 12, 256);
    WriteLittleEndianU32(bytes, 16, 1024);
    WriteLittleEndianU32(bytes, 20, 4096);
    WriteLittleEndianU32(bytes, 28, 0);
    WriteLittleEndianU32(bytes, 76, 32);
    WriteLittleEndianU32(bytes, 80, 0x41);
    WriteLittleEndianU32(bytes, 84, 0);
    WriteLittleEndianU32(bytes, 88, 32);
    WriteLittleEndianU32(bytes, 92, 0x00ff0000);
    WriteLittleEndianU32(bytes, 96, 0x0000ff00);
    WriteLittleEndianU32(bytes, 100, 0x000000ff);
    WriteLittleEndianU32(bytes, 104, 0xff000000);
    WriteLittleEndianU32(bytes, 108, 0x1000);
    bytes[128] = marker;
    return bytes;
}

struct ResolverState
{
    rex::Result<rex::system::AssetOverlayResolution> result =
        rex::Err<rex::system::AssetOverlayResolution>(
            rex::ErrorCategory::NotFound, "missing");
    std::string key;
    size_t      max_bytes     = 0;
    size_t      package_count = 0;
};

ResolverState resolver_state;

rex::Result<rex::system::AssetOverlayResolution> FakeResolver(
    std::span<const rex::system::AssetOverlayPackage> packages,
    std::string_view                                  key,
    size_t                                            max_bytes)
{
    resolver_state.key           = std::string(key);
    resolver_state.max_bytes     = max_bytes;
    resolver_state.package_count = packages.size();
    return resolver_state.result;
}

void TestValidator()
{
    auto data = MakeLogoDds(0x11);
    Require(rerevved::main_menu_logo::IsValidLogoDds(data),
            "valid legacy logo DDS rejected");

    data[4] = 0;
    Require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "invalid DDS magic accepted");
    data[4] = 124;

    WriteLittleEndianU32(data, 28, 1);
    Require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "mipmapped DDS accepted");
    WriteLittleEndianU32(data, 28, 0);

    WriteLittleEndianU32(data, 84, 0x30315844);
    Require(!rerevved::main_menu_logo::IsValidLogoDds(data),
            "DX10 DDS accepted");
}

void TestMissingFallsBack()
{
    rerevved::main_menu_logo::ResetForTests();
    resolver_state.result = rex::Err<rex::system::AssetOverlayResolution>(
        rex::ErrorCategory::NotFound, "missing");

    std::vector<rex::system::AssetOverlayPackage> packages(1);
    Require(!rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &FakeResolver),
            "missing asset selected");
    rerevved::main_menu_logo::Selection selection;
    Require(!rerevved::main_menu_logo::TryGetSelection(selection),
            "missing asset retained a payload");
    Require(resolver_state.key == rerevved::main_menu_logo::kAssetKey &&
                resolver_state.max_bytes == rerevved::main_menu_logo::kLogoDdsSize &&
                resolver_state.package_count == 1,
            "resolver adapter arguments changed");
}

void TestMalformedWinnerDoesNotFallThrough()
{
    rerevved::main_menu_logo::ResetForTests();
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id = "top.mod";
    resolved.bytes      = MakeLogoDds(0x21);
    resolved.bytes.resize(128);
    resolved.shadowed_package_ids = { "lower.mod" };
    resolver_state.result         = std::move(resolved);

    std::vector<rex::system::AssetOverlayPackage> packages(2);
    Require(!rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &FakeResolver),
            "malformed winner accepted");
    rerevved::main_menu_logo::Selection selection;
    Require(!rerevved::main_menu_logo::TryGetSelection(selection),
            "malformed winner retained a payload");
}

void TestValidWinnerIsRetained()
{
    rerevved::main_menu_logo::ResetForTests();
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id           = "top.mod";
    resolved.bytes                = MakeLogoDds(0x42);
    resolved.shadowed_package_ids = { "lower.mod", "retail" };
    resolver_state.result         = std::move(resolved);

    std::vector<rex::system::AssetOverlayPackage> packages(3);
    Require(rerevved::main_menu_logo::ResolveSelectedLogo(
                packages, &FakeResolver),
            "valid winner was rejected");

    rerevved::main_menu_logo::Selection selection;
    Require(rerevved::main_menu_logo::TryGetSelection(selection) &&
                selection.payload && selection.payload->at(128) == 0x42 &&
                selection.package_id == "top.mod" &&
                selection.shadowed_package_ids.size() == 2,
            "valid winner was not retained with provenance");
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
