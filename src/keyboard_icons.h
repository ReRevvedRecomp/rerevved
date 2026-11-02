// Keyboard prompts (A.dds, LB.dds, ...) use the Xelu Light keycap of the first bound key with art.
// scripts/build_keyboard_icons.py generates one 64x64 DDS per SDK key name.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::keyboard_icons
{

inline constexpr uint32_t kIconSize    = 64;
inline constexpr uint32_t kIconDdsSize = 16512u;

using Payload = std::shared_ptr<const std::vector<uint8_t>>;

struct Slot
{
    std::string_view fileName;
    std::string_view keybind;
    uint32_t         width;
    uint32_t         height;
};

// Stick and D-pad groups have no single key and are not slots
// The DUP, DDOWN, DLEFT and DRIGHT labels draw the generic DPAD image, so those prompts stay retail.
inline constexpr std::array<Slot, 16> kSlots{ {
    { "a.dds", "keybind_a", 64, 64 },
    { "b.dds", "keybind_b", 64, 64 },
    { "x.dds", "keybind_x", 64, 64 },
    { "y.dds", "keybind_y", 64, 64 },
    { "lb.dds", "keybind_left_shoulder", 128, 64 },
    { "rb.dds", "keybind_right_shoulder", 128, 64 },
    { "lt.dds", "keybind_left_trigger", 64, 128 },
    { "rt.dds", "keybind_right_trigger", 64, 128 },
    { "l3.dds", "keybind_lstick_press", 64, 64 },
    { "r3.dds", "keybind_rstick_press", 64, 64 },
    { "dup.dds", "keybind_dpad_up", 64, 64 },
    { "ddown.dds", "keybind_dpad_down", 64, 64 },
    { "dleft.dds", "keybind_dpad_left", 64, 64 },
    { "dright.dds", "keybind_dpad_right", 64, 64 },
    { "startbutton.dds", "keybind_start", 64, 64 },
    { "backbutton.dds", "keybind_back", 64, 64 },
} };

inline constexpr size_t kMaxFileNameLength = 15;

// Matched in any case
std::optional<size_t> FindSlot(std::string_view requestedName) noexcept;

std::span<const std::string_view> ShippedKeys() noexcept;

// First comma-separated alternative with shipped art
std::optional<std::string_view> PrimaryKey(std::string_view binding) noexcept;

// Exactly the legacy uncompressed 64x64 B8G8R8A8 file the generator writes.
bool IsValidIconDds(std::span<const uint8_t> data);

bool IsValidCanvasDds(std::span<const uint8_t> data, uint32_t width, uint32_t height);

// Each canvas dimension is at least 64 and an even number of pixels above it. Empty when invalid.
std::vector<uint8_t> ComposeCanvas(std::span<const uint8_t> icon, uint32_t width, uint32_t height);

// Returns the binding of a keybind cvar; empty when it is unset.
using BindingLookup = std::function<std::string(std::string_view keybind)>;

// Retain <dir>/<SdkName>.dds per slot for the process
size_t Prepare(const std::filesystem::path& dir, const BindingLookup& lookup);

struct Prepared
{
    Payload          payload;
    std::string_view key;
};

bool TryGetPrepared(std::string_view requestedName, Prepared& prepared);

bool AnyPrepared();

void ResetForTests();

} // namespace rerevved::keyboard_icons
