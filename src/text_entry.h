#pragma once

// Pure state; the Options page owns drawing and input.

#include <cstddef>
#include <string>
#include <string_view>

namespace rerevved::text_entry
{

// Charsets mirror the registry setter to avoid refused typed/pasted values.
enum class Charset
{
    kName,   // letters, digits, space, - _ .
    kInvite, // letters, digits, - and space: the separators the SDK ignores
    kDigits, // decimal digits only (a number typed whole)
};

struct Rules
{
    size_t  maxLength = 0;
    Charset charset   = Charset::kName;
};

bool CharAllowed(Charset charset, char c);

// Pasted text without surrounding ASCII whitespace or one pair of matching quotes. The view points into `text`.
std::string_view TrimView(std::string_view text);
std::string      TrimPasted(std::string_view text);

// Zero the whole storage before clearing credential text.
void Wipe(std::string& text);

std::string Masked(std::string_view text, size_t visible);

class Editor
{
public:
    explicit Editor(Rules rules, std::string initial = {});
    ~Editor();
    Editor(const Editor&)            = delete;
    Editor& operator=(const Editor&) = delete;

    const std::string& Text() const
    {
        return text_;
    }

    const Rules& GetRules() const
    {
        return rules_;
    }

    // False when the character is not allowed or the text is full.
    bool Append(char c);
    // Appends trimmed text; false with no change if any character is invalid or it does not fit.
    bool Paste(std::string_view text);
    bool Backspace();
    void Clear();

    // Typing or pasting replaces the selection; Backspace removes it; other edits clear it.
    void SelectAll();

    bool Selected() const
    {
        return selected_;
    }

private:
    Rules       rules_;
    std::string text_;
    bool        selected_ = false;
};

enum class KeyKind
{
    kChar,
    kSpace,
    kShift,
    kPaste,
    kClear,
    kDelete,
    kDone,
};

struct Key
{
    KeyKind kind = KeyKind::kChar;
    char    ch   = 0; // lower case for a letter
};

enum class Layout
{
    kFull,
    kDigits,
};

Layout LayoutFor(Charset charset);

// Cursor wraps at every edge and keeps relative position across unequal rows.
class Keyboard
{
public:
    static constexpr size_t kRowCount = 5;

    explicit Keyboard(Layout layout = Layout::kFull)
    : layout_(layout)
    {
    }

    static size_t RowCountOf(Layout layout);
    static size_t RowSize(size_t row, Layout layout = Layout::kFull);
    static Key    KeyAt(size_t row, size_t col, Layout layout = Layout::kFull);

    Layout GetLayout() const
    {
        return layout_;
    }

    size_t RowCount() const
    {
        return RowCountOf(layout_);
    }

    size_t Row() const
    {
        return row_;
    }

    size_t Col() const
    {
        return col_;
    }

    Key Current() const
    {
        return KeyAt(row_, col_, layout_);
    }

    void Move(int dx, int dy);
    void Set(size_t row, size_t col);

    bool Shifted() const
    {
        return shifted_;
    }

    void ToggleShift()
    {
        shifted_ = !shifted_;
    }

    // Character typed, uppercase while shifted; 0 for non-character keys.
    char CharOf(const Key& key) const;
    bool Enabled(const Key& key, Charset charset) const;

private:
    Layout layout_  = Layout::kFull;
    size_t row_     = 0;
    size_t col_     = 0;
    bool   shifted_ = false;
};

} // namespace rerevved::text_entry
