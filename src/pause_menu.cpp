#include "pause_menu.h"

#include "title_text.h"

#include <mutex>
#include <utility>
#include <vector>

namespace rerevved::pause_menu
{

namespace
{

std::mutex  gHostMutex;
HostActions gHost;

} // namespace

std::optional<Variant> VariantForReturn(uint32_t returnAddress)
{
    if (returnAddress == kFullShowReturn)
        return Variant::kFull;
    if (returnAddress == kAlternateShowReturn)
        return Variant::kAlternate;
    return std::nullopt;
}

Choice ResolveRow(Variant variant, int32_t row)
{
    const auto retailRows = static_cast<int32_t>(RetailRowCount(variant));
    if (row >= 0 && row < retailRows)
        return { Choice::Kind::kRetail, row };
    if (row == retailRows)
        return { Choice::Kind::kExitGame };
    return {};
}

size_t RetailRowCount(Variant variant)
{
    return variant == Variant::kFull ? 5 : 3;
}

std::optional<std::string> BuildList(Variant variant, std::string_view formattedRetail, uint32_t languageId)
{
    std::vector<std::string_view> lines;
    for (size_t start = 0; start < formattedRetail.size();)
    {
        const size_t end = formattedRetail.find('\n', start);
        if (end == std::string_view::npos)
            return std::nullopt;
        lines.push_back(formattedRetail.substr(start, end - start));
        start = end + 1;
    }
    if (lines.size() != 1 + RetailRowCount(variant) || lines.front().empty())
        return std::nullopt;

    using title_text::Id;
    const std::string_view lastRetail = lines.back();
    std::string            exitText   = title_text::Text(Id::kExitGameRow, languageId);
    if (lastRetail.ends_with('.') && !exitText.ends_with('.'))
        exitText.push_back('.');

    std::string result;
    result.reserve(formattedRetail.size() + 64);
    result.append(formattedRetail);
    result.append(" " + exitText + "\n");
    return result;
}

uint32_t QuitText(uint32_t sessionMode, bool sessionPresent, bool ranked)
{
    if (sessionPresent && sessionMode == 2)
        return kQuitTextSession;
    return ranked ? kQuitTextRanked : kQuitTextUnsaved;
}

void SetHostActions(HostActions actions)
{
    std::lock_guard lock(gHostMutex);
    gHost = std::move(actions);
}

// Hold the lock so SetHostActions({}) waits for running actions at shutdown.
void RequestQuit()
{
    std::lock_guard lock(gHostMutex);
    if (gHost.quit)
        gHost.quit();
}

} // namespace rerevved::pause_menu
