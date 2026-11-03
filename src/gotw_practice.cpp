#include "gotw_practice.h"

namespace rerevved::gotw
{
namespace
{

constexpr uint8_t kStandardRules      = 1;
constexpr uint8_t kCustomWeek         = 2;
constexpr uint8_t kCustomCivilization = 4;
constexpr uint8_t kKnownFlags         = 7;

} // namespace

bool IsValid(const PracticeBlock& block) noexcept
{
    return IsValidWeek(block.week) && block.seed != 0 && (!block.customWeek || block.seed == SeedForWeek(block.week)) &&
           IsValidCivilization(block.civilization) && (block.dominationMinimum == 5 || block.dominationMinimum == 10);
}

void AppendPracticeBlock(std::vector<uint8_t>& out, const PracticeBlock& block)
{
    out.push_back(static_cast<uint8_t>((block.standardRules ? kStandardRules : 0) | (block.customWeek ? kCustomWeek : 0) |
                                       (block.customCivilization ? kCustomCivilization : 0)));
    for (uint32_t value : { block.week, block.seed })
    {
        for (int shift : { 24, 16, 8, 0 })
        {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    out.push_back(static_cast<uint8_t>(block.civilization));
    out.push_back(static_cast<uint8_t>(block.dominationMinimum));
}

std::optional<PracticeBlock> ReadPracticeBlock(std::span<const uint8_t> bytes, size_t& offset) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < kPracticeBlockSize || (bytes[offset] & ~kKnownFlags) != 0)
    {
        return std::nullopt;
    }
    const uint8_t flags = bytes[offset];
    PracticeBlock block;
    block.standardRules      = (flags & kStandardRules) != 0;
    block.customWeek         = (flags & kCustomWeek) != 0;
    block.customCivilization = (flags & kCustomCivilization) != 0;
    for (size_t i = 0; i < 4; ++i)
    {
        block.week = (block.week << 8) | bytes[offset + 1 + i];
        block.seed = (block.seed << 8) | bytes[offset + 5 + i];
    }
    block.civilization      = bytes[offset + 9];
    block.dominationMinimum = bytes[offset + 10];
    if (!IsValid(block))
    {
        return std::nullopt;
    }
    offset += kPracticeBlockSize;
    return block;
}

std::optional<PracticeBlock> CurrentPracticeBlock(uint32_t effectiveCivilization, int32_t dominationMinimum) noexcept
{
    const auto current = ForGame();
    if (!current.practice)
    {
        return std::nullopt;
    }
    PracticeBlock block;
    block.standardRules      = StandardRulesForGame();
    block.customWeek         = current.customWeek;
    block.customCivilization = current.customCivilization;
    block.week               = current.week;
    block.seed               = current.seed;
    block.civilization       = effectiveCivilization;
    block.dominationMinimum  = dominationMinimum;
    if (!IsValid(block))
    {
        return std::nullopt;
    }
    return block;
}

bool NeedsIsolationMarker(bool hasBlock) noexcept
{
    return !hasBlock && ForGame().Isolates();
}

Selection SelectionFor(const PracticeBlock& block) noexcept
{
    Selection selection;
    selection.customWeek         = block.customWeek;
    selection.customCivilization = block.customCivilization;
    selection.nativeWeek         = block.week;
    selection.week               = block.week;
    selection.seed               = block.seed;
    selection.civilization       = block.civilization;
    return selection;
}

void RestorePractice(const PracticeBlock& block) noexcept
{
    RestoreForLoad(SelectionFor(block), block.standardRules);
}

LoadKind ClassifyLoad(uint32_t nativeWeek, const std::optional<PracticeBlock>& saved, bool sidecarClaimsPractice, bool resumable) noexcept
{
    if (saved && saved->week == nativeWeek && resumable)
    {
        return LoadKind::Practice;
    }
    return saved || sidecarClaimsPractice ? LoadKind::Isolated : LoadKind::Ordinary;
}

} // namespace rerevved::gotw
