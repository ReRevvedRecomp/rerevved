#include "text_entry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rerevved::text_entry
{

namespace
{

bool isAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

constexpr std::array<std::string_view, 4> kCharRows{ "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_." };
constexpr std::array<KeyKind, 3>          kDigitActionRow{ KeyKind::kClear, KeyKind::kDelete, KeyKind::kDone };
constexpr std::array<KeyKind, 6>          kActionRow{ KeyKind::kShift, KeyKind::kSpace, KeyKind::kPaste, KeyKind::kClear, KeyKind::kDelete, KeyKind::kDone };

size_t mapColumn(size_t col, size_t from, size_t to)
{
    if (from <= 1 || to <= 1)
        return 0;
    return static_cast<size_t>(std::lround(static_cast<double>(col) * static_cast<double>(to - 1) / static_cast<double>(from - 1)));
}

} // namespace

bool CharAllowed(Charset charset, char c)
{
    if (charset == Charset::kDigits)
        return c >= '0' && c <= '9';
    if (isAlnum(c) || c == '-' || c == ' ')
        return true;
    return charset == Charset::kName && (c == '_' || c == '.');
}

std::string_view TrimView(std::string_view text)
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
        text = text.substr(1, text.size() - 2);
    return text;
}

std::string TrimPasted(std::string_view text)
{
    return std::string(TrimView(text));
}

void Wipe(std::string& text)
{
    // Volatile wipes cannot be optimized away; clear capacity, not just live text.
    volatile char* bytes = text.data();
    for (size_t i = 0; i < text.capacity(); ++i)
        bytes[i] = 0;
    text.clear();
}

std::string Masked(std::string_view text, size_t visible)
{
    const size_t hidden = text.size() > visible ? text.size() - visible : 0;
    std::string  result(text);
    std::fill_n(result.begin(), hidden, '*');
    return result;
}

Editor::Editor(Rules rules, std::string initial)
: rules_(rules)
, text_(std::move(initial))
{
    if (rules_.charset == Charset::kDigits && !text_.empty())
    {
        const size_t first = text_.find_first_not_of('0');
        text_.erase(0, first == std::string::npos ? text_.size() - 1 : first);
    }
    if (text_.size() > rules_.maxLength)
        text_.resize(rules_.maxLength);
    // Allocate once so growth cannot leave a secret copy behind.
    text_.reserve(rules_.maxLength);
}

Editor::~Editor()
{
    Wipe(text_);
}

bool Editor::Append(char c)
{
    if (!CharAllowed(rules_.charset, c))
        return false;
    if (selected_)
        Clear();
    // Replace a leading 0 with the next digit so capacity counts significant digits.
    if (rules_.charset == Charset::kDigits && text_ == "0")
    {
        text_[0] = c;
        return true;
    }
    if (text_.size() >= rules_.maxLength)
        return false;
    text_.push_back(c);
    return true;
}

void Editor::SelectAll()
{
    selected_ = !text_.empty();
}

bool Editor::Paste(std::string_view text)
{
    const std::string_view trimmed = TrimView(text);
    const size_t           kept    = selected_ ? 0 : text_.size();
    if (trimmed.empty())
        return false;
    if (!std::all_of(trimmed.begin(), trimmed.end(), [&](char c)
                     {
                         return CharAllowed(rules_.charset, c);
                     }))
        return false;
    if (rules_.charset == Charset::kDigits)
    {
        std::string  next  = (selected_ ? std::string() : text_) + std::string(trimmed);
        const size_t first = next.find_first_not_of('0');
        next.erase(0, first == std::string::npos ? next.size() - 1 : first);
        if (next.size() > rules_.maxLength)
            return false;
        selected_ = false;
        text_     = std::move(next);
        return true;
    }
    if (kept + trimmed.size() > rules_.maxLength)
        return false;
    if (selected_)
        Clear();
    text_.append(trimmed);
    return true;
}

bool Editor::Backspace()
{
    if (text_.empty())
        return false;
    if (selected_)
    {
        Clear();
        return true;
    }
    text_.back() = 0;
    text_.pop_back();
    return true;
}

void Editor::Clear()
{
    selected_ = false;
    Wipe(text_);
}

Layout LayoutFor(Charset charset)
{
    return charset == Charset::kDigits ? Layout::kDigits : Layout::kFull;
}

namespace
{

size_t charRowCount(Layout layout)
{
    return layout == Layout::kDigits ? 1 : kCharRows.size();
}

} // namespace

size_t Keyboard::RowCountOf(Layout layout)
{
    return charRowCount(layout) + 1;
}

size_t Keyboard::RowSize(size_t row, Layout layout)
{
    if (row < charRowCount(layout))
        return kCharRows[row].size();
    return layout == Layout::kDigits ? kDigitActionRow.size() : kActionRow.size();
}

Key Keyboard::KeyAt(size_t row, size_t col, Layout layout)
{
    row = std::min(row, RowCountOf(layout) - 1);
    col = std::min(col, RowSize(row, layout) - 1);
    if (row < charRowCount(layout))
        return { KeyKind::kChar, kCharRows[row][col] };
    return { layout == Layout::kDigits ? kDigitActionRow[col] : kActionRow[col], 0 };
}

void Keyboard::Move(int dx, int dy)
{
    if (dy != 0)
    {
        const size_t from  = RowSize(row_, layout_);
        const long   count = static_cast<long>(RowCount());
        const long   next  = (static_cast<long>(row_) + dy) % count;
        row_               = static_cast<size_t>(next < 0 ? next + count : next);
        col_               = mapColumn(col_, from, RowSize(row_, layout_));
    }
    if (dx != 0)
    {
        const long size = static_cast<long>(RowSize(row_, layout_));
        const long next = (static_cast<long>(col_) + dx) % size;
        col_            = static_cast<size_t>(next < 0 ? next + size : next);
    }
}

void Keyboard::Set(size_t row, size_t col)
{
    row_ = std::min(row, RowCount() - 1);
    col_ = std::min(col, RowSize(row_, layout_) - 1);
}

char Keyboard::CharOf(const Key& key) const
{
    switch (key.kind)
    {
        case KeyKind::kChar:
            return shifted_ && key.ch >= 'a' && key.ch <= 'z' ? static_cast<char>(key.ch - 'a' + 'A') : key.ch;
        case KeyKind::kSpace:
            return ' ';
        default:
            return 0;
    }
}

bool Keyboard::Enabled(const Key& key, Charset charset) const
{
    const char c = CharOf(key);
    return c == 0 || CharAllowed(charset, c);
}

} // namespace rerevved::text_entry
