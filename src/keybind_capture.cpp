#include "keybind_capture.h"

#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace rerevved::keybind_capture
{

namespace
{

using VK = rex::ui::VirtualKey;

VK asKey(uint16_t virtualKey)
{
    return static_cast<VK>(virtualKey);
}

VK modifierOf(VK key)
{
    switch (key)
    {
        case VK::kShift:
        case VK::kLShift:
        case VK::kRShift:
            return VK::kShift;
        case VK::kControl:
        case VK::kLControl:
        case VK::kRControl:
            return VK::kControl;
        case VK::kMenu:
        case VK::kLMenu:
        case VK::kRMenu:
            return VK::kMenu;
        default:
            return VK::kNone;
    }
}

} // namespace

Kind Classify(uint16_t virtualKey)
{
    const VK key = asKey(virtualKey);
    if (modifierOf(key) != VK::kNone)
        return Kind::kModifier;
    if (key == VK::kEscape)
        return Kind::kCancel;
    return KeyName(virtualKey).empty() ? Kind::kUnbindable : Kind::kKey;
}

std::string KeyName(uint16_t virtualKey)
{
    const VK key = asKey(virtualKey);
    if (key == VK::kNone || virtualKey <= static_cast<uint16_t>(VK::kXButton2) || key == VK::kEscape || modifierOf(key) != VK::kNone)
        return {};
    std::string name = rex::ui::VirtualKeyToString(key);
    // Return and the keypad Enter share a key; the registry may name either.
    if (name == "NumpadEnter")
        name = "Return";
    return name;
}

std::string ModifierName(uint16_t virtualKey)
{
    const VK modifier = modifierOf(asKey(virtualKey));
    return modifier == VK::kNone ? std::string() : rex::ui::VirtualKeyToString(modifier);
}

std::string Binding(uint16_t virtualKey, bool shift, bool ctrl, bool alt, bool chordsAllowed)
{
    std::string name = KeyName(virtualKey);
    if (name.empty() || !chordsAllowed)
        return name;
    std::string prefix;
    if (shift)
        prefix += "Shift+";
    if (ctrl)
        prefix += "Ctrl+";
    if (alt)
        prefix += "Alt+";
    return prefix + name;
}

bool Gate::OnDown(uint16_t virtualKey, bool repeat, bool shift, bool ctrl, bool alt, bool capturing)
{
    if (held_.contains(virtualKey))
    {
        if (repeat)
            return true;
        // Focus loss can hide a release; a fresh press clears the waiting modifier.
        held_.erase(virtualKey);
        if (virtualKey == modifier_)
        {
            modifier_      = 0;
            modifierClean_ = false;
        }
    }
    if (!capturing)
        return false;
    // Repeats are not presses; pre-capture held keys keep their state elsewhere.
    if (repeat)
        return true;
    held_.insert(virtualKey);
    if (Classify(virtualKey) == Kind::kModifier)
    {
        if (modifier_ == 0)
        {
            modifier_      = virtualKey;
            modifierClean_ = true;
        }
        else
        {
            modifierClean_ = false;
        }
    }
    else
    {
        modifierClean_ = false;
        queue_.push_back({ virtualKey, shift, ctrl, alt, false });
    }
    return true;
}

bool Gate::OnUp(uint16_t virtualKey, bool capturing)
{
    if (held_.erase(virtualKey) == 0)
        return false;
    if (virtualKey == modifier_)
    {
        if (modifierClean_ && capturing)
            queue_.push_back({ virtualKey, false, false, false, true });
        modifier_      = 0;
        modifierClean_ = false;
    }
    return true;
}

namespace
{

// Text events lack key identity; only held printable keys repeat text.
bool typesText(uint16_t key)
{
    return key == 0x20 || (key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5A) || (key >= 0x60 && key <= 0x6F) ||
           (key >= 0xBA && key <= 0xC0) || (key >= 0xDB && key <= 0xDF);
}

} // namespace

bool Gate::HoldsText() const
{
    for (const uint16_t key : held_)
    {
        if (typesText(key))
            return true;
    }
    return false;
}

std::optional<Key> Gate::Take()
{
    if (queue_.empty())
        return std::nullopt;
    const Key key = queue_.front();
    queue_.pop_front();
    return key;
}

void Gate::Discard()
{
    queue_.clear();
    modifier_      = 0;
    modifierClean_ = false;
}

void Listener::OnKeyDown(rex::ui::KeyEvent& event)
{
    if (gate_.OnDown(static_cast<uint16_t>(event.virtual_key()), event.prev_state(), event.is_shift_pressed(), event.is_ctrl_pressed(), event.is_alt_pressed(), capturing()))
        event.set_handled(true);
}

void Listener::OnKeyUp(rex::ui::KeyEvent& event)
{
    if (gate_.OnUp(static_cast<uint16_t>(event.virtual_key()), capturing()))
        event.set_handled(true);
}

void Listener::OnKeyChar(rex::ui::KeyEvent& event)
{
    // Captured text repeats stay suppressed until release, even after focus moves.
    if (capturing() || gate_.HoldsText())
        event.set_handled(true);
}

} // namespace rerevved::keybind_capture
