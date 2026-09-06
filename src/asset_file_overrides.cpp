#include "asset_file_overrides_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::asset_file_overrides
{

namespace
{

constexpr std::string_view kLogoPath  = "GFX_MainMenu_logo.dds";
constexpr uint32_t         kDdsWidth  = 1024;
constexpr uint32_t         kDdsHeight = 256;

struct StoredOverride
{
    std::string provider_id;
    std::string rule_id;
    std::string path;
    Payload     payload;
};

std::shared_mutex           registry_mutex;
std::vector<StoredOverride> registry;

bool IsRuleIdValid(const char* value, size_t capacity)
{
    const void* terminator = std::memchr(value, '\0', capacity);
    if (!terminator || value[0] == '\0')
    {
        return false;
    }

    for (const char* current = value; *current != '\0'; ++current)
    {
        const char c = *current;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
        {
            return false;
        }
    }
    const char first = value[0];
    return (first >= 'a' && first <= 'z') ||
           (first >= '0' && first <= '9');
}

bool IsReservedZero(const uint32_t (&reserved)[8])
{
    return std::all_of(std::begin(reserved), std::end(reserved), [](uint32_t value)
                       {
                           return value == 0;
                       });
}

bool IsPrintableAsciiPath(const char* value, size_t& length)
{
    const void* terminator = std::memchr(
        value, '\0', REREVVED_ASSET_FILE_OVERRIDE_PATH_CAPACITY);
    if (!terminator)
    {
        return false;
    }

    length = static_cast<size_t>(
        static_cast<const char*>(terminator) - value);
    if (length == 0)
    {
        return false;
    }
    for (size_t index = 0; index < length; ++index)
    {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (c < 0x20 || c > 0x7e)
        {
            return false;
        }
    }
    return true;
}

uint32_t ReadLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) |
           (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

bool IsValidLogoDds(const uint8_t* data, uint32_t data_size)
{
    if (!data || data_size != REREVVED_ASSET_FILE_OVERRIDE_LOGO_DDS_SIZE ||
        std::memcmp(data, "DDS ", 4) != 0)
    {
        return false;
    }

    if (ReadLittleEndianU32(data + 4) != 124 ||
        ReadLittleEndianU32(data + 12) != kDdsHeight ||
        ReadLittleEndianU32(data + 16) != kDdsWidth ||
        ReadLittleEndianU32(data + 24) != 0 ||
        ReadLittleEndianU32(data + 28) != 0)
    {
        return false;
    }

    // A nonzero FourCC includes the DX10 extension and every compressed DDS
    // format. ABI 1 accepts only the legacy uncompressed pixel format.
    if (ReadLittleEndianU32(data + 76) != 32 ||
        ReadLittleEndianU32(data + 80) != 0x41 ||
        ReadLittleEndianU32(data + 84) != 0 ||
        ReadLittleEndianU32(data + 88) != 32 ||
        ReadLittleEndianU32(data + 92) != 0x00ff0000 ||
        ReadLittleEndianU32(data + 96) != 0x0000ff00 ||
        ReadLittleEndianU32(data + 100) != 0x000000ff ||
        ReadLittleEndianU32(data + 104) != 0xff000000)
    {
        return false;
    }

    constexpr uint32_t kDdsCapsComplex = 0x00000008;
    constexpr uint32_t kDdsCapsMipmap  = 0x00400000;
    return (ReadLittleEndianU32(data + 108) &
            (kDdsCapsComplex | kDdsCapsMipmap)) == 0 &&
           ReadLittleEndianU32(data + 112) == 0;
}

} // namespace

bool TryGetPayload(std::string_view path, Payload& payload)
{
    std::shared_lock lock(registry_mutex);
    const auto       found = std::find_if(
        registry.begin(), registry.end(), [&](const StoredOverride& entry)
        {
            return entry.path == path;
        });
    if (found == registry.end())
    {
        payload.reset();
        return false;
    }
    payload = found->payload;
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registry_mutex);
    registry.clear();
}

} // namespace rerevved::asset_file_overrides

static_assert(offsetof(ReRevvedAssetFileOverride, struct_size) == 0);
static_assert(offsetof(ReRevvedAssetFileOverride, provider_id) == 4);
static_assert(offsetof(ReRevvedAssetFileOverride, rule_id) == 68);
static_assert(offsetof(ReRevvedAssetFileOverride, path) == 132);

extern "C" uint32_t ReRevvedAssetFileOverridesAbiVersion(void)
{
    return REREVVED_ASSET_FILE_OVERRIDES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterAssetFileOverride(
    const ReRevvedAssetFileOverride* rule)
{
    using namespace rerevved::asset_file_overrides;

    if (!rule ||
        rule->struct_size < sizeof(ReRevvedAssetFileOverride) ||
        !IsRuleIdValid(rule->provider_id,
                       REREVVED_ASSET_FILE_OVERRIDE_PROVIDER_ID_CAPACITY) ||
        !IsRuleIdValid(rule->rule_id,
                       REREVVED_ASSET_FILE_OVERRIDE_RULE_ID_CAPACITY) ||
        !IsReservedZero(rule->reserved) || !rule->data)
    {
        return REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT;
    }

    size_t path_length = 0;
    if (!IsPrintableAsciiPath(rule->path, path_length))
    {
        return REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT;
    }
    if (std::string_view(rule->path, path_length) != kLogoPath)
    {
        return REREVVED_ASSET_FILE_OVERRIDES_ERR_UNSUPPORTED_PATH;
    }
    if (!IsValidLogoDds(static_cast<const uint8_t*>(rule->data),
                        rule->data_size))
    {
        return REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD;
    }

    try
    {
        auto copied = std::make_shared<const std::vector<uint8_t>>(
            static_cast<const uint8_t*>(rule->data),
            static_cast<const uint8_t*>(rule->data) + rule->data_size);
        StoredOverride candidate{
            std::string(rule->provider_id),
            std::string(rule->rule_id),
            std::string(rule->path, path_length),
            std::move(copied),
        };

        std::unique_lock lock(registry_mutex);
        const auto       duplicate = std::find_if(
            registry.begin(), registry.end(), [&](const StoredOverride& entry)
            {
                return entry.path == candidate.path;
            });
        if (duplicate != registry.end())
        {
            return REREVVED_ASSET_FILE_OVERRIDES_ERR_DUPLICATE_PATH;
        }
        registry.push_back(std::move(candidate));
    }
    catch (...)
    {
        return REREVVED_ASSET_FILE_OVERRIDES_ERR_INTERNAL;
    }
    return REREVVED_ASSET_FILE_OVERRIDES_OK;
}
