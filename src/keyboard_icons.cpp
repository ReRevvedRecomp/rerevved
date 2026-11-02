#include "keyboard_icons.h"

#include <algorithm>
#include <array>
#include <cctype>
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

std::shared_mutex                   selectionMutex;
std::array<Prepared, kSlots.size()> selected;

constexpr size_t kHeaderSize = 128;

uint32_t readLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) | (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

void writeLittleEndianU32(uint8_t* destination, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
    {
        destination[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

size_t canvasDdsSize(uint32_t width, uint32_t height)
{
    return kHeaderSize + size_t{ width } * height * 4;
}

void clearSelection()
{
    std::unique_lock lock(selectionMutex);
    selected = {};
}

std::string_view trimSpaces(std::string_view text) noexcept
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }
    return text;
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

bool equalsLower(std::string_view text, std::string_view lower) noexcept
{
    if (text.size() != lower.size())
    {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != static_cast<unsigned char>(lower[i]))
        {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<uint8_t>> readIcon(const std::filesystem::path& path)
{
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    if (error || size != kIconDdsSize)
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' is missing or the wrong size", path.string());
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(kIconDdsSize);
    std::ifstream        file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' could not be read", path.string());
        return std::nullopt;
    }
    if (!IsValidIconDds(bytes))
    {
        REXLOG_WARN("ButtonIcons: keyboard art '{}' is malformed", path.string());
        return std::nullopt;
    }
    return bytes;
}

} // namespace

std::span<const std::string_view> ShippedKeys() noexcept
{
    return kKeys;
}

std::optional<size_t> FindSlot(std::string_view requestedName) noexcept
{
    for (size_t i = 0; i < kSlots.size(); ++i)
    {
        if (equalsLower(requestedName, kSlots[i].fileName))
        {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> PrimaryKey(std::string_view binding) noexcept
{
    while (true)
    {
        const size_t           comma       = binding.find(',');
        const std::string_view alternative = trimSpaces(binding.substr(0, comma));
        if (!alternative.empty() && alternative.find('+') == std::string_view::npos)
        {
            // SDK exact-modifier matching cannot activate bare Shift, Control or Alt (no left or right names).
            const bool modifier = alternative == "Shift" || alternative == "Control" || alternative == "Alt";
            if (const auto key = shippedKey(alternative); key && !modifier)
            {
                return key;
            }
        }
        if (comma == std::string_view::npos)
        {
            return std::nullopt;
        }
        binding.remove_prefix(comma + 1);
    }
}

bool IsValidIconDds(std::span<const uint8_t> data)
{
    return IsValidCanvasDds(data, kIconSize, kIconSize);
}

bool IsValidCanvasDds(std::span<const uint8_t> data, uint32_t width, uint32_t height)
{
    if (data.data() == nullptr || width == 0 || height == 0 || width > 4096 || height > 4096 ||
        data.size() != canvasDdsSize(width, height) || std::memcmp(data.data(), "DDS ", 4) != 0)
    {
        return false;
    }

    const auto* bytes = data.data();
    // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT, no mip chain, no volume.
    if (readLittleEndianU32(bytes + 4) != 124 ||
        readLittleEndianU32(bytes + 8) != 0x100f ||
        readLittleEndianU32(bytes + 12) != height ||
        readLittleEndianU32(bytes + 16) != width ||
        readLittleEndianU32(bytes + 20) != width * 4 ||
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

std::vector<uint8_t> ComposeCanvas(std::span<const uint8_t> icon, uint32_t width, uint32_t height)
{
    if (!IsValidIconDds(icon) || width < kIconSize || height < kIconSize || width > 4096 || height > 4096 ||
        (width - kIconSize) % 2 != 0 || (height - kIconSize) % 2 != 0)
    {
        return {};
    }

    // Transparent black is all zero bytes; only size fields differ from the icon.
    std::vector<uint8_t> canvas(canvasDdsSize(width, height), 0);
    std::memcpy(canvas.data(), icon.data(), kHeaderSize);
    writeLittleEndianU32(canvas.data() + 12, height);
    writeLittleEndianU32(canvas.data() + 16, width);
    writeLittleEndianU32(canvas.data() + 20, width * 4);

    const size_t left = (width - kIconSize) / 2;
    const size_t top  = (height - kIconSize) / 2;
    for (size_t row = 0; row < kIconSize; ++row)
    {
        std::memcpy(canvas.data() + kHeaderSize + ((top + row) * width + left) * 4,
                    icon.data() + kHeaderSize + row * kIconSize * 4,
                    kIconSize * 4);
    }
    return canvas;
}

size_t Prepare(const std::filesystem::path& dir, const BindingLookup& lookup)
{
    clearSelection();

    std::array<Prepared, kSlots.size()> prepared{};
    size_t                              count = 0;
    for (size_t i = 0; i < kSlots.size(); ++i)
    {
        const Slot&       slot    = kSlots[i];
        const std::string binding = lookup ? lookup(slot.keybind) : std::string();
        const auto        key     = PrimaryKey(binding);
        if (!key)
        {
            REXLOG_INFO("ButtonIcons: {} '{}' has no single key with art; keeping the retail {} icon", slot.keybind, binding, slot.fileName);
            continue;
        }

        const auto icon   = readIcon(dir / (std::string(*key) + ".dds"));
        auto       canvas = icon ? ComposeCanvas(*icon, slot.width, slot.height) : std::vector<uint8_t>();
        if (canvas.empty())
        {
            REXLOG_WARN("ButtonIcons: keeping the retail {} icon", slot.fileName);
            continue;
        }
        prepared[i] = { std::make_shared<const std::vector<uint8_t>>(std::move(canvas)), *key };
        ++count;
        REXLOG_INFO("ButtonIcons: keyboard {} is {}", slot.fileName, *key);
    }

    std::unique_lock lock(selectionMutex);
    selected = std::move(prepared);
    return count;
}

bool TryGetPrepared(std::string_view requestedName, Prepared& prepared)
{
    const auto slot = FindSlot(requestedName);
    if (!slot)
    {
        return false;
    }
    std::shared_lock lock(selectionMutex);
    prepared = selected[*slot];
    return static_cast<bool>(prepared.payload);
}

bool AnyPrepared()
{
    std::shared_lock lock(selectionMutex);
    return std::any_of(selected.begin(), selected.end(), [](const Prepared& entry)
                       {
                           return static_cast<bool>(entry.payload);
                       });
}

void ResetForTests()
{
    clearSelection();
}

} // namespace rerevved::keyboard_icons
