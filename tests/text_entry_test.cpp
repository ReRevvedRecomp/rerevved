#include "text_entry.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

namespace
{

namespace te = rerevved::text_entry;

constexpr te::Rules kName{ 15, te::Charset::kName };
constexpr te::Rules kInvite{ 1024, te::Charset::kInvite };

} // namespace

TEST_CASE("text_entry_charsets_match_the_registry_setters", "[ctest][text-entry]")
{
    for (const char c : std::string("aZ09- "))
    {
        CHECK(te::CharAllowed(te::Charset::kName, c));
        CHECK(te::CharAllowed(te::Charset::kInvite, c));
    }
    for (const char c : std::string("_."))
    {
        CHECK(te::CharAllowed(te::Charset::kName, c));
        CHECK_FALSE(te::CharAllowed(te::Charset::kInvite, c));
    }
    for (const char c : std::string("!@/\\\"'\t"))
    {
        CHECK_FALSE(te::CharAllowed(te::Charset::kName, c));
        CHECK_FALSE(te::CharAllowed(te::Charset::kInvite, c));
    }
    CHECK_FALSE(te::CharAllowed(te::Charset::kName, static_cast<char>(0xe9)));
}

TEST_CASE("text_entry_trims_pasted_text_like_the_registry", "[ctest][text-entry]")
{
    CHECK(te::TrimPasted("  abc \r\n") == "abc");
    CHECK(te::TrimPasted("\"abc-1\"") == "abc-1");
    CHECK(te::TrimPasted(" 'abc' ") == "abc");
    CHECK(te::TrimPasted("\"abc'") == "\"abc'");
    CHECK(te::TrimPasted("\"\"abc\"\"") == "\"abc\"");
    CHECK(te::TrimPasted("   ").empty());
}

TEST_CASE("text_entry_masks_all_but_the_tail", "[ctest][text-entry]")
{
    CHECK(te::Masked("abcdefgh", 4) == "****efgh");
    CHECK(te::Masked("abc", 4) == "abc");
    CHECK(te::Masked("", 4).empty());
    CHECK(te::Masked("abcd", 0) == "****");
}

TEST_CASE("text_entry_editor_appends_within_the_rules", "[ctest][text-entry]")
{
    te::Editor editor(kName, "Aesh");
    CHECK(editor.Text() == "Aesh");
    CHECK(editor.Append('_'));
    CHECK_FALSE(editor.Append('!'));
    CHECK(editor.Text() == "Aesh_");

    editor.Clear();
    for (int i = 0; i < 15; ++i)
        CHECK(editor.Append('a'));
    CHECK_FALSE(editor.Append('b'));
    CHECK(editor.Text().size() == 15);

    CHECK(editor.Backspace());
    CHECK(editor.Text().size() == 14);
    editor.Clear();
    CHECK_FALSE(editor.Backspace());

    CHECK(te::Editor(kName, "0123456789abcdefgh").Text() == "0123456789abcde");
}

TEST_CASE("text_entry_paste_is_all_or_nothing", "[ctest][text-entry]")
{
    te::Editor editor(kInvite);
    CHECK(editor.Paste("  \"abc-123\"\n"));
    CHECK(editor.Text() == "abc-123");
    CHECK(editor.Paste("-ab cd"));
    CHECK(editor.Text() == "abc-123-ab cd");
    editor.Clear();
    CHECK(editor.Paste("abc-123"));
    CHECK_FALSE(editor.Paste("good_bad"));
    CHECK_FALSE(editor.Paste("ok!"));
    CHECK_FALSE(editor.Paste("   "));
    CHECK(editor.Text() == "abc-123");

    te::Editor name(kName, "1234567890");
    CHECK_FALSE(name.Paste("abcdef"));
    CHECK(name.Text() == "1234567890");
    CHECK(name.Paste("abcde"));
    CHECK(name.Text() == "1234567890abcde");
}

TEST_CASE("text_entry_keyboard_grid_has_every_character_a_name_needs", "[ctest][text-entry]")
{
    std::set<char> typed;
    for (size_t r = 0; r < te::Keyboard::kRowCount; ++r)
    {
        for (size_t c = 0; c < te::Keyboard::RowSize(r); ++c)
        {
            const te::Key key = te::Keyboard::KeyAt(r, c);
            if (key.kind == te::KeyKind::kChar)
                typed.insert(key.ch);
        }
    }
    for (char c = 'a'; c <= 'z'; ++c)
        CHECK(typed.contains(c));
    for (char c = '0'; c <= '9'; ++c)
        CHECK(typed.contains(c));
    for (const char c : std::string("-_."))
        CHECK(typed.contains(c));

    std::set<te::KeyKind> actions;
    for (size_t c = 0; c < te::Keyboard::RowSize(te::Keyboard::kRowCount - 1); ++c)
        actions.insert(te::Keyboard::KeyAt(te::Keyboard::kRowCount - 1, c).kind);
    CHECK(actions == std::set<te::KeyKind>{ te::KeyKind::kShift, te::KeyKind::kSpace, te::KeyKind::kPaste, te::KeyKind::kClear, te::KeyKind::kDelete, te::KeyKind::kDone });
}

TEST_CASE("text_entry_keyboard_cursor_wraps_and_keeps_its_place_between_rows", "[ctest][text-entry]")
{
    te::Keyboard board;
    CHECK(board.Row() == 0);
    CHECK(board.Col() == 0);
    board.Move(-1, 0);
    CHECK(board.Col() == 9);
    board.Move(1, 0);
    CHECK(board.Col() == 0);
    board.Move(0, -1);
    CHECK(board.Row() == te::Keyboard::kRowCount - 1);
    board.Move(0, 1);
    CHECK(board.Row() == 0);

    board.Set(0, 9);
    board.Move(0, -1);
    CHECK(board.Row() == te::Keyboard::kRowCount - 1);
    CHECK(board.Col() == te::Keyboard::RowSize(te::Keyboard::kRowCount - 1) - 1);
    board.Move(0, 1);
    CHECK(board.Row() == 0);
    CHECK(board.Col() == 9);

    board.Set(99, 99);
    CHECK(board.Row() == te::Keyboard::kRowCount - 1);
    CHECK(board.Col() == te::Keyboard::RowSize(te::Keyboard::kRowCount - 1) - 1);
}

TEST_CASE("text_entry_keyboard_shift_and_disabled_keys", "[ctest][text-entry]")
{
    te::Keyboard  board;
    const te::Key q{ te::KeyKind::kChar, 'q' };
    const te::Key dot{ te::KeyKind::kChar, '.' };
    const te::Key space{ te::KeyKind::kSpace, 0 };
    CHECK(board.CharOf(q) == 'q');
    board.ToggleShift();
    CHECK(board.Shifted());
    CHECK(board.CharOf(q) == 'Q');
    CHECK(board.CharOf(dot) == '.');
    CHECK(board.CharOf(te::Key{ te::KeyKind::kShift, 0 }) == 0);
    CHECK(board.CharOf(space) == ' ');

    CHECK(board.Enabled(q, te::Charset::kInvite));
    CHECK_FALSE(board.Enabled(dot, te::Charset::kInvite));
    CHECK(board.Enabled(space, te::Charset::kInvite));
    CHECK(board.Enabled(dot, te::Charset::kName));
    CHECK(board.Enabled(space, te::Charset::kName));
    CHECK(board.Enabled(te::Key{ te::KeyKind::kPaste, 0 }, te::Charset::kInvite));
}

TEST_CASE("text_entry_takes_the_longest_canonical_invite", "[ctest][text-entry]")
{
    // "RXR1-" and 575 base-32 characters: the SDK's 359-byte payload.
    std::string code = "RXR1-";
    for (size_t i = 0; i < 575; ++i)
        code.push_back("ABCDEFGHJKMNPQRSTVWXYZ0123456789"[i % 32]);
    REQUIRE(code.size() == 580);
    te::Editor editor(kInvite);
    CHECK(editor.Paste(code));
    CHECK(editor.Text() == code);
    editor.Clear();
    std::string grouped;
    for (size_t i = 0; i < code.size(); i += 5)
        grouped += code.substr(i, 5) + (i % 10 == 0 ? "-" : " ");
    CHECK(grouped.size() > code.size());
    CHECK(editor.Paste(grouped));
}

TEST_CASE("text_entry_wipe_zeroes_the_storage_and_empties_the_string", "[ctest][text-entry]")
{
    std::string text(40, 'x');
    const char* data = text.data();
    te::Wipe(text);
    CHECK(text.empty());
    for (size_t i = 0; i < 40; ++i)
        CHECK(data[i] == 0);

    te::Editor editor(kName, "abc");
    CHECK(editor.Backspace());
    CHECK(editor.Text() == "ab");
    editor.Clear();
    CHECK(editor.Text().empty());
}

TEST_CASE("text_entry_digits_charset_and_keyboard_take_numbers_only", "[ctest][text-entry]")
{
    for (char c = '0'; c <= '9'; ++c)
        CHECK(te::CharAllowed(te::Charset::kDigits, c));
    for (const char c : std::string("aZ -_.+,"))
        CHECK_FALSE(te::CharAllowed(te::Charset::kDigits, c));

    const auto layout = te::LayoutFor(te::Charset::kDigits);
    CHECK(layout == te::Layout::kDigits);
    CHECK(te::LayoutFor(te::Charset::kName) == te::Layout::kFull);
    REQUIRE(te::Keyboard::RowCountOf(layout) == 2);
    CHECK(te::Keyboard::RowSize(0, layout) == 10);
    std::string digits;
    for (size_t c = 0; c < te::Keyboard::RowSize(0, layout); ++c)
        digits.push_back(te::Keyboard::KeyAt(0, c, layout).ch);
    CHECK(digits == "1234567890");
    std::set<te::KeyKind> actions;
    for (size_t c = 0; c < te::Keyboard::RowSize(1, layout); ++c)
        actions.insert(te::Keyboard::KeyAt(1, c, layout).kind);
    CHECK(actions == std::set<te::KeyKind>{ te::KeyKind::kClear, te::KeyKind::kDelete, te::KeyKind::kDone });

    te::Keyboard board(layout);
    board.Move(0, 1);
    CHECK(board.Row() == 1);
    board.Move(0, 1);
    CHECK(board.Row() == 0);
    board.Move(0, -1);
    CHECK(board.Row() == 1);
    board.Move(-1, 0);
    CHECK(board.Col() == 2);
    CHECK(board.Current().kind == te::KeyKind::kDone);

    te::Editor number({ 10, te::Charset::kDigits });
    CHECK(number.Append('7'));
    CHECK_FALSE(number.Append('x'));
    CHECK_FALSE(number.Paste("1a"));
    CHECK(number.Text() == "7");
}

TEST_CASE("text_entry_selected_text_is_replaced_by_the_next_edit", "[ctest][text-entry]")
{
    const te::Rules rules{ 10, te::Charset::kDigits };
    {
        te::Editor editor(rules, "900");
        CHECK_FALSE(editor.Selected());
        editor.SelectAll();
        CHECK(editor.Selected());
        CHECK(editor.Append('4'));
        CHECK(editor.Text() == "4");
        CHECK_FALSE(editor.Selected());
    }
    {
        te::Editor editor(rules, "900");
        editor.SelectAll();
        CHECK(editor.Paste("12"));
        CHECK(editor.Text() == "12");
    }
    {
        te::Editor editor(rules, "900");
        editor.SelectAll();
        CHECK(editor.Backspace());
        CHECK(editor.Text().empty());
        CHECK_FALSE(editor.Selected());
    }
    {
        te::Editor editor(rules, "900");
        editor.SelectAll();
        CHECK_FALSE(editor.Append('x'));
        CHECK(editor.Selected());
        CHECK(editor.Text() == "900");
    }
    {
        te::Editor empty(rules);
        empty.SelectAll();
        CHECK_FALSE(empty.Selected());
        te::Editor full(rules, "1234567890");
        full.SelectAll();
        CHECK(full.Append('5'));
        CHECK(full.Text() == "5");
    }
}

TEST_CASE("text_entry_digits_paste_drops_leading_zeros_before_the_limit", "[ctest][text-entry]")
{
    const te::Rules rules{ 4, te::Charset::kDigits };
    te::Editor      editor(rules);
    CHECK(editor.Paste("00000000042"));
    CHECK(editor.Text() == "42");
    editor.Clear();
    CHECK(editor.Paste("0000"));
    CHECK(editor.Text() == "0");
    editor.Clear();
    CHECK_FALSE(editor.Paste("0012345"));
    CHECK(editor.Text().empty());
    CHECK(editor.Append('1'));
    CHECK(editor.Paste("002"));
    CHECK(editor.Text() == "1002");
    te::Editor name({ 15, te::Charset::kName });
    CHECK(name.Paste("007"));
    CHECK(name.Text() == "007");
}

TEST_CASE("text_entry_digits_never_store_leading_zeros", "[ctest][text-entry]")
{
    const te::Rules rules{ 4, te::Charset::kDigits };
    {
        te::Editor editor(rules);
        for (const char c : std::string("0042"))
            CHECK(editor.Append(c));
        CHECK(editor.Text() == "42");
    }
    {
        te::Editor editor(rules);
        for (const char c : std::string("000000000042"))
            CHECK(editor.Append(c));
        CHECK(editor.Text() == "42");
    }
    {
        te::Editor padded(rules, "000000000042");
        CHECK(padded.Text() == "42");
        CHECK(padded.Append('7'));
        CHECK(padded.Text() == "427");
        te::Editor zeros(rules, "0000");
        CHECK(zeros.Text() == "0");
    }
    {
        te::Editor editor(rules);
        CHECK(editor.Append('0'));
        CHECK(editor.Text() == "0");
        CHECK(editor.Paste("00000000042"));
        CHECK(editor.Text() == "42");
    }
    {
        te::Editor editor(rules);
        CHECK(editor.Paste("000"));
        CHECK(editor.Text() == "0");
        CHECK(editor.Append('0'));
        CHECK(editor.Text() == "0");
        CHECK(editor.Backspace());
        CHECK(editor.Text().empty());
        CHECK(editor.Append('1'));
        CHECK(editor.Append('0'));
        CHECK(editor.Text() == "10");
    }
    {
        te::Editor editor(rules);
        CHECK(editor.Paste("0001234"));
        CHECK(editor.Text() == "1234");
        CHECK_FALSE(editor.Append('5'));
        CHECK_FALSE(editor.Paste("0"));
        CHECK(editor.Text() == "1234");
    }
}
