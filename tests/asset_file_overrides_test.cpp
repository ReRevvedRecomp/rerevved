#include "asset_file_overrides_registry.h"

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
    std::vector<uint8_t> bytes(
        REREVVED_ASSET_FILE_OVERRIDE_LOGO_DDS_SIZE, 0);
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

ReRevvedAssetFileOverride MakeRule(const char*                 provider,
                                   const char*                 rule_id,
                                   const char*                 path,
                                   const std::vector<uint8_t>& data)
{
    ReRevvedAssetFileOverride rule{};
    rule.struct_size = sizeof(rule);
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    std::memcpy(rule.path, path, std::strlen(path) + 1);
    rule.data      = data.data();
    rule.data_size = static_cast<uint32_t>(data.size());
    return rule;
}

void TestAbiLayout()
{
    static_assert(offsetof(ReRevvedAssetFileOverride, struct_size) == 0);
    static_assert(offsetof(ReRevvedAssetFileOverride, provider_id) == 4);
    static_assert(offsetof(ReRevvedAssetFileOverride, rule_id) == 68);
    static_assert(offsetof(ReRevvedAssetFileOverride, path) == 132);
    Require(ReRevvedAssetFileOverridesAbiVersion() ==
                REREVVED_ASSET_FILE_OVERRIDES_ABI_VERSION,
            "ABI version mismatch");
}

void TestValidation()
{
    rerevved::asset_file_overrides::ResetForTests();
    auto data = MakeLogoDds(0x11);
    auto rule = MakeRule("example.logo", "logo", "GFX_MainMenu_logo.dds", data);

    Require(ReRevvedRegisterAssetFileOverride(nullptr) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT,
            "null rule accepted");
    rule.struct_size--;
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT,
            "short rule accepted");
    rule.struct_size++;

    rule.provider_id[0] = 'E';
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT,
            "malformed provider accepted");
    rule.provider_id[0] = 'e';

    rule.path[3] = static_cast<char>(0x1f);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT,
            "non-printable path accepted");
    std::memcpy(rule.path, "GFX_MainMenu_logo.dds", 22);

    std::memcpy(rule.path, "other.dds", 10);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_UNSUPPORTED_PATH,
            "unsupported path accepted");
    std::memcpy(rule.path, "GFX_MainMenu_logo.dds", 22);

    rule.data_size--;
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "wrong payload size accepted");
    rule.data_size++;
    data[4] = 0;
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "invalid DDS magic accepted");
    data[4] = 124;
    WriteLittleEndianU32(data, 28, 1);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "mipmapped DDS accepted");
    WriteLittleEndianU32(data, 28, 0);
    WriteLittleEndianU32(data, 84, 0x30315844);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "DX10 DDS accepted");
    WriteLittleEndianU32(data, 84, 0);
    WriteLittleEndianU32(data, 24, 1);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "three-dimensional DDS accepted");
    WriteLittleEndianU32(data, 24, 0);
    WriteLittleEndianU32(data, 92, 0);
    Require(ReRevvedRegisterAssetFileOverride(&rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD,
            "wrong channel mask accepted");
}

void TestCopyAndPriority()
{
    rerevved::asset_file_overrides::ResetForTests();
    auto first      = MakeLogoDds(0x21);
    auto second     = MakeLogoDds(0x42);
    auto first_rule = MakeRule(
        "top.mod", "logo-first", "GFX_MainMenu_logo.dds", first);
    auto second_rule = MakeRule(
        "native.mod", "logo-second", "GFX_MainMenu_logo.dds", second);

    Require(ReRevvedRegisterAssetFileOverride(&first_rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_OK,
            "first valid registration failed");
    const uint8_t original_marker = first[128];
    first[128]                    = 0;

    rerevved::asset_file_overrides::Payload payload;
    Require(rerevved::asset_file_overrides::TryGetPayload(
                "GFX_MainMenu_logo.dds", payload) &&
                payload && payload->at(128) == original_marker,
            "registration did not copy caller bytes");

    Require(ReRevvedRegisterAssetFileOverride(&second_rule) ==
                REREVVED_ASSET_FILE_OVERRIDES_ERR_DUPLICATE_PATH,
            "lower-priority duplicate path was accepted");
    Require(rerevved::asset_file_overrides::TryGetPayload(
                "GFX_MainMenu_logo.dds", payload) &&
                payload->at(128) == original_marker,
            "duplicate registration replaced the first payload");
}

} // namespace

int main()
{
    TestAbiLayout();
    TestValidation();
    TestCopyAndPriority();
    return 0;
}
