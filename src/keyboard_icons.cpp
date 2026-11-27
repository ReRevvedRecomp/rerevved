#include "keyboard_icons.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
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

std::shared_mutex                    selectionMutex;
std::array<Prepared, kPreparedCount> selected;

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

std::optional<size_t> FindPrepared(std::string_view requestedName) noexcept
{
    if (const auto slot = FindSlot(requestedName))
    {
        return slot;
    }
    for (size_t i = 0; i < kClusters.size(); ++i)
    {
        if (equalsLower(requestedName, kClusters[i].fileName))
        {
            return kSlots.size() + i;
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

std::vector<uint8_t> ComposeCluster(const std::array<std::span<const uint8_t>, 4>& icons)
{
    // One key above a row of three.
    constexpr size_t                                   kCell = 21;
    constexpr size_t                                   kTop  = (kIconSize - 2 * kCell) / 2;
    constexpr std::array<std::pair<size_t, size_t>, 4> kOrigins{ {
        { kCell, kTop },
        { 0, kTop + kCell },
        { kCell, kTop + kCell },
        { 2 * kCell, kTop + kCell },
    } };
    // Clear source pixels kept around each keycap, so neighbors do not touch.
    constexpr size_t kMargin = 1;

    size_t left = kIconSize, top = kIconSize, right = 0, bottom = 0;
    for (const auto& icon : icons)
    {
        if (!IsValidIconDds(icon))
        {
            return {};
        }
        for (size_t y = 0; y < kIconSize; ++y)
        {
            for (size_t x = 0; x < kIconSize; ++x)
            {
                if (icon[kHeaderSize + (y * kIconSize + x) * 4 + 3] != 0)
                {
                    left   = std::min(left, x);
                    top    = std::min(top, y);
                    right  = std::max(right, x + 1);
                    bottom = std::max(bottom, y + 1);
                }
            }
        }
    }
    if (left >= right)
    {
        return {};
    }

    // One square crop for all four keeps their relative size. It may extend past the icon.
    const double side    = static_cast<double>(std::max(right - left, bottom - top) + 2 * kMargin);
    const double originX = (static_cast<double>(left + right) - side) / 2;
    const double originY = (static_cast<double>(top + bottom) - side) / 2;
    const double step    = side / kCell;
    const auto   size    = static_cast<ptrdiff_t>(kIconSize);

    std::vector<uint8_t> cluster(kIconDdsSize, 0);
    std::memcpy(cluster.data(), icons[0].data(), kHeaderSize);
    for (size_t i = 0; i < icons.size(); ++i)
    {
        const uint8_t* source = icons[i].data() + kHeaderSize;
        for (size_t cellY = 0; cellY < kCell; ++cellY)
        {
            for (size_t cellX = 0; cellX < kCell; ++cellX)
            {
                // Area average weighted by alpha, so transparent texels add no dark fringe.
                const double x0 = originX + static_cast<double>(cellX) * step;
                const double y0 = originY + static_cast<double>(cellY) * step;
                const double x1 = x0 + step;
                const double y1 = y0 + step;
                double       sum[4]{};
                for (auto y = static_cast<ptrdiff_t>(std::floor(y0)); y < static_cast<ptrdiff_t>(std::ceil(y1)); ++y)
                {
                    for (auto x = static_cast<ptrdiff_t>(std::floor(x0)); x < static_cast<ptrdiff_t>(std::ceil(x1)); ++x)
                    {
                        if (x < 0 || y < 0 || x >= size || y >= size)
                        {
                            continue;
                        }
                        const double   weight = (std::min(static_cast<double>(x + 1), x1) - std::max(static_cast<double>(x), x0)) *
                                                (std::min(static_cast<double>(y + 1), y1) - std::max(static_cast<double>(y), y0));
                        const uint8_t* texel  = source + (static_cast<size_t>(y) * kIconSize + static_cast<size_t>(x)) * 4;
                        const double   alpha  = weight * texel[3];
                        sum[0] += alpha * texel[0];
                        sum[1] += alpha * texel[1];
                        sum[2] += alpha * texel[2];
                        sum[3] += alpha;
                    }
                }
                if (sum[3] <= 0)
                {
                    continue;
                }
                uint8_t* out = cluster.data() + kHeaderSize + ((kOrigins[i].second + cellY) * kIconSize + kOrigins[i].first + cellX) * 4;
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    out[channel] = static_cast<uint8_t>(std::lround(sum[channel] / sum[3]));
                }
                out[3] = static_cast<uint8_t>(std::lround(sum[3] / (step * step)));
            }
        }
    }
    return cluster;
}

size_t Prepare(const std::filesystem::path& dir, const BindingLookup& lookup)
{
    clearSelection();

    std::array<Prepared, kPreparedCount> prepared{};
    size_t                               count = 0;
    for (size_t i = 0; i < kSlots.size(); ++i)
    {
        const Slot&       slot    = kSlots[i];
        const std::string binding = lookup ? lookup(slot.keybind) : std::string();
        const auto        key     = PrimaryKey(binding);
        if (!key)
        {
            REXLOG_DEBUG("ButtonIcons: {} '{}' has no single key with art; keeping the retail {} icon", slot.keybind, binding, slot.fileName);
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
        REXLOG_DEBUG("ButtonIcons: keyboard {} is {}", slot.fileName, *key);
    }

    for (size_t i = 0; i < kClusters.size(); ++i)
    {
        const Cluster&                      cluster = kClusters[i];
        std::array<std::vector<uint8_t>, 4> icons;
        std::string_view                    upKey;
        bool                                complete = true;
        for (size_t direction = 0; direction < icons.size() && complete; ++direction)
        {
            const auto key  = PrimaryKey(lookup ? lookup(cluster.keybinds[direction]) : std::string());
            auto       icon = key ? readIcon(dir / (std::string(*key) + ".dds")) : std::nullopt;
            complete        = icon.has_value();
            if (complete)
            {
                icons[direction] = std::move(*icon);
                upKey            = direction == 0 ? *key : upKey;
            }
        }
        auto image = complete ? ComposeCluster({ icons[0], icons[1], icons[2], icons[3] }) : std::vector<uint8_t>();
        if (image.empty())
        {
            REXLOG_DEBUG("ButtonIcons: {} needs four single keys with art; keeping the retail icon", cluster.fileName);
            continue;
        }
        prepared[kSlots.size() + i] = { std::make_shared<const std::vector<uint8_t>>(std::move(image)), upKey };
        ++count;
    }

    std::unique_lock lock(selectionMutex);
    selected = std::move(prepared);
    return count;
}

bool TryGetPrepared(std::string_view requestedName, Prepared& prepared)
{
    const auto index = FindPrepared(requestedName);
    if (!index)
    {
        return false;
    }
    std::shared_lock lock(selectionMutex);
    prepared = selected[*index];
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
