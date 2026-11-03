#include "keybind_capture.h"

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/virtual_key.h>

namespace kc = rerevved::keybind_capture;
using VK     = rex::ui::VirtualKey;

namespace
{

uint16_t vk(VK key)
{
    return static_cast<uint16_t>(key);
}

bool down(kc::Gate& gate, VK key, bool capturing, bool repeat = false, bool shift = false, bool ctrl = false, bool alt = false)
{
    return gate.OnDown(vk(key), repeat, shift, ctrl, alt, capturing);
}

bool up(kc::Gate& gate, VK key, bool capturing)
{
    return gate.OnUp(vk(key), capturing);
}

} // namespace

TEST_CASE("keybind_capture_names_keys_as_the_registry_does", "[ctest][keybind-capture]")
{
    CHECK(kc::KeyName(vk(VK::kW)) == "W");
    CHECK(kc::KeyName(vk(VK::k5)) == "5");
    CHECK(kc::KeyName(vk(VK::kF3)) == "F3");
    CHECK(kc::KeyName(vk(VK::kF13)) == "F13");
    CHECK(kc::KeyName(vk(VK::kF24)) == "F24");
    CHECK(kc::KeyName(vk(VK::kNumpad4)) == "Numpad4");
    CHECK(kc::KeyName(vk(VK::kSpace)) == "Space");
    CHECK(kc::KeyName(vk(VK::kUp)) == "Up");
    CHECK(kc::KeyName(vk(VK::kOem1)) == "Semicolon");
    CHECK(kc::KeyName(vk(VK::kOem7)) == "Quote");
    CHECK(kc::KeyName(vk(VK::kOem3)) == "Backtick");
    CHECK(kc::KeyName(vk(VK::kBack)) == "Backspace");
}

TEST_CASE("keybind_capture_names_enter_as_return_never_the_keypad_alias", "[ctest][keybind-capture]")
{
    // The registry maps Return and NumpadEnter to one key and may name either;
    CHECK(kc::KeyName(vk(VK::kReturn)) == "Return");
    CHECK(kc::Binding(vk(VK::kReturn), false, false, false, false) == "Return");
    CHECK(kc::Binding(vk(VK::kReturn), true, false, false, true) == "Shift+Return");
}

TEST_CASE("keybind_capture_classifies_what_a_waiting_row_does_with_a_key", "[ctest][keybind-capture]")
{
    CHECK(kc::Classify(vk(VK::kW)) == kc::Kind::kKey);
    CHECK(kc::Classify(vk(VK::kF20)) == kc::Kind::kKey);
    for (const VK modifier : { VK::kShift, VK::kLShift, VK::kRShift, VK::kControl, VK::kLControl, VK::kRControl, VK::kMenu, VK::kLMenu, VK::kRMenu })
        CHECK(kc::Classify(vk(modifier)) == kc::Kind::kModifier);
    CHECK(kc::Classify(vk(VK::kEscape)) == kc::Kind::kCancel);
    CHECK(kc::Classify(vk(VK::kDecimal)) == kc::Kind::kUnbindable);
    CHECK(kc::Classify(vk(VK::kLWin)) == kc::Kind::kUnbindable);
    CHECK(kc::Classify(vk(VK::kLButton)) == kc::Kind::kUnbindable);
    CHECK(kc::Classify(vk(VK::kNone)) == kc::Kind::kUnbindable);
    CHECK(kc::KeyName(vk(VK::kDecimal)).empty());
    CHECK(kc::Binding(vk(VK::kDecimal), true, false, false, true).empty());
}

TEST_CASE("keybind_capture_names_a_modifier_for_a_hotkey", "[ctest][keybind-capture]")
{
    CHECK(kc::ModifierName(vk(VK::kShift)) == "Shift");
    CHECK(kc::ModifierName(vk(VK::kRShift)) == "Shift");
    CHECK(kc::ModifierName(vk(VK::kLControl)) == "Control");
    CHECK(kc::ModifierName(vk(VK::kRMenu)) == "Alt");
    CHECK(kc::ModifierName(vk(VK::kW)).empty());
}

TEST_CASE("keybind_capture_builds_chords_for_controller_keys_only", "[ctest][keybind-capture]")
{
    CHECK(kc::Binding(vk(VK::kUp), true, false, false, true) == "Shift+Up");
    CHECK(kc::Binding(vk(VK::kW), false, true, false, true) == "Ctrl+W");
    CHECK(kc::Binding(vk(VK::kW), true, true, true, true) == "Shift+Ctrl+Alt+W");
    CHECK(kc::Binding(vk(VK::kW), false, false, false, true) == "W");
    CHECK(kc::Binding(vk(VK::kF7), true, true, false, false) == "F7");
}

TEST_CASE("keybind_capture_gate_takes_a_key_until_it_comes_up", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    CHECK_FALSE(down(gate, VK::kF1, false));
    CHECK_FALSE(up(gate, VK::kF1, false));
    CHECK_FALSE(gate.Pending());

    REQUIRE(down(gate, VK::kF1, true));
    REQUIRE(gate.Pending());
    const auto key = gate.Take();
    REQUIRE(key);
    CHECK(key->virtualKey == vk(VK::kF1));
    CHECK_FALSE(key->modifierOnly);
    CHECK_FALSE(gate.Pending());

    // Held-key repeats and release stay in the gate so drivers are not triggered.
    CHECK(down(gate, VK::kF1, false, true));
    CHECK(down(gate, VK::kF1, false, true));
    CHECK_FALSE(gate.Pending());
    CHECK(up(gate, VK::kF1, false));
    CHECK_FALSE(down(gate, VK::kF1, false));
    CHECK_FALSE(up(gate, VK::kF1, false));
}

TEST_CASE("keybind_capture_gate_ignores_a_key_held_from_before_the_capture", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    CHECK(down(gate, VK::kReturn, true, true));
    CHECK_FALSE(gate.Pending());
    CHECK_FALSE(up(gate, VK::kReturn, true));
}

TEST_CASE("keybind_capture_gate_forgets_a_release_it_never_saw", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    REQUIRE(down(gate, VK::kF2, true));
    CHECK_FALSE(down(gate, VK::kF2, false));
    CHECK_FALSE(up(gate, VK::kF2, false));
}

TEST_CASE("keybind_capture_gate_keeps_escape_and_chord_modifiers_for_the_row", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    REQUIRE(down(gate, VK::kShift, true, false, true));
    REQUIRE(down(gate, VK::kUp, true, false, true));
    const auto key = gate.Take();
    REQUIRE(key);
    CHECK(key->virtualKey == vk(VK::kUp));
    CHECK(key->shift);
    CHECK_FALSE(gate.Pending());
    CHECK(up(gate, VK::kShift, true));
    CHECK_FALSE(gate.Pending());
    CHECK(up(gate, VK::kUp, true));

    // Capture Escape before ImGui so a focused window behind cannot close.
    REQUIRE(down(gate, VK::kEscape, true));
    const auto esc = gate.Take();
    REQUIRE(esc);
    CHECK(esc->virtualKey == vk(VK::kEscape));
    CHECK(up(gate, VK::kEscape, false));
}

TEST_CASE("keybind_capture_gate_reads_a_modifier_pressed_and_released_alone", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    REQUIRE(down(gate, VK::kControl, true, false, false, true));
    CHECK_FALSE(gate.Pending());
    REQUIRE(up(gate, VK::kControl, true));
    const auto key = gate.Take();
    REQUIRE(key);
    CHECK(key->modifierOnly);
    CHECK(key->virtualKey == vk(VK::kControl));

    REQUIRE(down(gate, VK::kShift, true));
    REQUIRE(down(gate, VK::kMenu, true));
    CHECK(up(gate, VK::kMenu, true));
    CHECK(up(gate, VK::kShift, true));
    CHECK_FALSE(gate.Pending());

    REQUIRE(down(gate, VK::kShift, true));
    CHECK(up(gate, VK::kShift, false));
    CHECK_FALSE(gate.Pending());
}

TEST_CASE("keybind_capture_gate_discard_drops_what_was_not_read", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    REQUIRE(down(gate, VK::kA, true));
    REQUIRE(down(gate, VK::kShift, true));
    gate.Discard();
    CHECK_FALSE(gate.Pending());
    CHECK(up(gate, VK::kShift, true));
    CHECK_FALSE(gate.Pending());
}

TEST_CASE("keybind_capture_gate_keeps_a_held_keys_text_until_it_comes_up", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    CHECK_FALSE(gate.HoldsText());
    REQUIRE(down(gate, VK::kW, true));
    CHECK(gate.HoldsText());
    CHECK(up(gate, VK::kW, false));
    CHECK_FALSE(gate.HoldsText());

    for (const VK quiet : { VK::kShift, VK::kF5, VK::kUp, VK::kEscape, VK::kBack })
    {
        REQUIRE(down(gate, quiet, true));
        CHECK_FALSE(gate.HoldsText());
        CHECK(up(gate, quiet, false));
    }
    for (const VK loud : { VK::kSpace, VK::k7, VK::kNumpad3, VK::kOem1 })
    {
        REQUIRE(down(gate, loud, true));
        CHECK(gate.HoldsText());
        CHECK(up(gate, loud, false));
    }
}

TEST_CASE("keybind_capture_gate_binds_a_modifier_alone_after_a_lost_release", "[ctest][keybind-capture]")
{
    kc::Gate gate;
    REQUIRE(down(gate, VK::kShift, true));
    REQUIRE(down(gate, VK::kShift, true));
    REQUIRE(up(gate, VK::kShift, true));
    const auto key = gate.Take();
    REQUIRE(key);
    CHECK(key->modifierOnly);

    REQUIRE(down(gate, VK::kControl, true));
    CHECK_FALSE(down(gate, VK::kControl, false));
    REQUIRE(down(gate, VK::kShift, true));
    CHECK(up(gate, VK::kShift, true));
    REQUIRE(gate.Take());
}
