#pragma once

// Encodes Keybind keys for F1 and the keyboard-and-mouse driver; owns the keyboard while waiting.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>

#include <rex/ui/window_listener.h>

namespace rerevved::keybind_capture
{

enum class Kind
{
    kKey,
    kModifier,
    kCancel,
    kUnbindable, // no name the SDK can store (keypad decimal, the Windows key)
};

Kind Classify(uint16_t virtualKey);

// Registry key name or empty; keypad Enter uses Return for keyboard art and icon lookup.
std::string KeyName(uint16_t virtualKey);

std::string ModifierName(uint16_t virtualKey);

// Controller bindings prefix Shift+, Ctrl+, Alt+ in that order (exact driver match); overlay hotkeys omit modifiers.
std::string Binding(uint16_t virtualKey, bool shift, bool ctrl, bool alt, bool chordsAllowed);

struct Key
{
    uint16_t virtualKey   = 0;
    bool     shift        = false;
    bool     ctrl         = false;
    bool     alt          = false;
    bool     modifierOnly = false;
};

// Owns captured keys and repeats until release; no driver, page, ImGui or overlay hotkey sees them.
class Gate
{
public:
    // True when the event is the gate's. `repeat` is the key's earlier state.
    bool OnDown(uint16_t virtualKey, bool repeat, bool shift, bool ctrl, bool alt, bool capturing);
    bool OnUp(uint16_t virtualKey, bool capturing);

    // Captured printable keys own repeated characters until released; modifiers, function keys and arrows do not.
    bool HoldsText() const;

    bool Pending() const
    {
        return !queue_.empty();
    }

    std::optional<Key> Take();
    void               Discard();

private:
    std::set<uint16_t> held_;
    std::deque<Key>    queue_;
    uint16_t           modifier_      = 0;
    bool               modifierClean_ = false;
};

class Listener final : public rex::ui::WindowInputListener
{
public:
    explicit Listener(std::function<bool()> capturing)
    : capturing_(std::move(capturing))
    {
    }

    void OnKeyDown(rex::ui::KeyEvent& event) override;
    void OnKeyUp(rex::ui::KeyEvent& event) override;
    void OnKeyChar(rex::ui::KeyEvent& event) override;

    Gate& Keys()
    {
        return gate_;
    }

private:
    bool capturing() const
    {
        return capturing_ && capturing_();
    }

    std::function<bool()> capturing_;
    Gate                  gate_;
};

constexpr size_t kListenerZ = 1000;

} // namespace rerevved::keybind_capture
