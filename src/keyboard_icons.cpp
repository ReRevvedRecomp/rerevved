#include "keyboard_icons.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>

#include <rex/logging.h>

namespace rerevved::keyboard_icons
{

namespace
{

constexpr uint32_t kIconSize = 64;

// Must list the same SDK key names as KEYS in scripts/build_keyboard_icons.py.
constexpr std::array<std::string_view, 80> kKeys{ {
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
    "Backtick",
    "Minus",
    "Plus",
    "Semicolon",
    "Slash",
    "LBracket",
    "RBracket",
    "Quote",
    "Escape",
    "Return",
    "Space",
    "Tab",
    "Backspace",
    "Delete",
    "Insert",
    "Home",
    "End",
    "PageUp",
    "PageDown",
    "Left",
    "Right",
    "Up",
    "Down",
    "Shift",
    "Control",
    "Alt",
    "CapsLock",
    "NumLock",
    "PrintScreen",
    "LMB",
    "RMB",
    "MMB",
} };

std::shared_mutex selectionMutex;
Payload           selectedIcon;
std::string_view  selectedKey;

uint32_t readLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) | (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

void clearSelection()
{
    std::unique_lock lock(selectionMutex);
    selectedIcon = {};
    selectedKey  = {};
}

std::string_view firstAlternative(std::string_view binding) noexcept
{
    binding = binding.substr(0, binding.find(','));
    while (!binding.empty() && binding.front() == ' ')
    {
        binding.remove_prefix(1);
    }
    while (!binding.empty() && binding.back() == ' ')
    {
        binding.remove_suffix(1);
    }
    return binding;
}

std::optional<std::string_view> shippedKey(std::string_view name) noexcept
{
    const auto found = std::find(kKeys.begin(), kKeys.end(), name);
    if (found == kKeys.end())
    {
        return std::nullopt;
    }
    return *found;
}

} // namespace

std::span<const std::string_view> ShippedKeys() noexcept
{
    return kKeys;
}

std::optional<std::string_view> PrimaryKey(std::string_view binding) noexcept
{
    const std::string_view first = firstAlternative(binding);
    if (first.empty() || first.find('+') != std::string_view::npos)
    {
        return std::nullopt;
    }
    return shippedKey(first);
}

bool IsValidIconDds(std::span<const uint8_t> data)
{
    if (data.data() == nullptr || data.size() != kIconDdsSize || std::memcmp(data.data(), "DDS ", 4) != 0)
    {
        return false;
    }

    const auto* bytes = data.data();
    // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT, no mip chain, no volume.
    if (readLittleEndianU32(bytes + 4) != 124 ||
        readLittleEndianU32(bytes + 8) != 0x100f ||
        readLittleEndianU32(bytes + 12) != kIconSize ||
        readLittleEndianU32(bytes + 16) != kIconSize ||
        readLittleEndianU32(bytes + 20) != kIconSize * 4 ||
        readLittleEndianU32(bytes + 24) != 0 ||
        readLittleEndianU32(bytes + 28) != 0)
    {
        return false;
    }

    // Reject nonzero FourCC (including DX10): only legacy uncompressed DDS is valid.
    return readLittleEndianU32(bytes + 76) == 32 &&
           readLittleEndianU32(bytes + 80) == 0x41 &&
           readLittleEndianU32(bytes + 84) == 0 &&
           readLittleEndianU32(bytes + 88) == 32 &&
           readLittleEndianU32(bytes + 92) == 0x00ff0000 &&
           readLittleEndianU32(bytes + 96) == 0x0000ff00 &&
           readLittleEndianU32(bytes + 100) == 0x000000ff &&
           readLittleEndianU32(bytes + 104) == 0xff000000 &&
           readLittleEndianU32(bytes + 108) == 0x1000 &&
           readLittleEndianU32(bytes + 112) == 0 &&
           readLittleEndianU32(bytes + 116) == 0 &&
           readLittleEndianU32(bytes + 120) == 0;
}

bool Prepare(const std::filesystem::path& dir, std::string_view binding)
{
    clearSelection();

    const std::string_view first = firstAlternative(binding);
    if (first.empty())
    {
        REXLOG_INFO("ButtonIcons: keybind_a has no key; keeping the retail A icon");
        return false;
    }
    if (first.find('+') != std::string_view::npos)
    {
        REXLOG_INFO("ButtonIcons: keybind_a starts with chord '{}'; keeping the retail A icon", first);
        return false;
    }
    const auto key = shippedKey(first);
    if (!key)
    {
        REXLOG_INFO("ButtonIcons: no keyboard art for keybind_a key '{}'; keeping the retail A icon", first);
        return false;
    }

    const std::filesystem::path path = dir / (std::string(*key) + ".dds");
    std::error_code             error;
    const auto                  size = std::filesystem::file_size(path, error);
    if (error || size != kIconDdsSize)
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' is missing or the wrong size; keeping the retail A icon", path.string());
        return false;
    }
    std::vector<uint8_t> bytes(kIconDdsSize);
    std::ifstream        file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' could not be read; keeping the retail A icon", path.string());
        return false;
    }
    if (!IsValidIconDds(bytes))
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' is malformed; keeping the retail A icon", path.string());
        return false;
    }

    auto payload = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    {
        std::unique_lock lock(selectionMutex);
        selectedIcon = std::move(payload);
        selectedKey  = *key;
    }
    REXLOG_INFO("ButtonIcons: keyboard A icon is {}", *key);
    return true;
}

bool TryGetPayload(Payload& payload)
{
    std::shared_lock lock(selectionMutex);
    payload = selectedIcon;
    return static_cast<bool>(payload);
}

std::string_view PreparedKey()
{
    std::shared_lock lock(selectionMutex);
    return selectedKey;
}

void ResetForTests()
{
    clearSelection();
}

} // namespace rerevved::keyboard_icons
