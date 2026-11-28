#include "retail_dlc.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <fmt/format.h>

namespace rerevved::retail_dlc
{

namespace
{

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

bool printable(char c)
{
    return c > 0x20 && c < 0x7F;
}

} // namespace

std::string ParseToken(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last  = text.find_last_not_of(" \t\r\n");
    const auto token = text.substr(first, last - first + 1);
    if (!std::all_of(token.begin(), token.end(), printable))
    {
        return {};
    }
    return std::string(token);
}

std::string CheckFileName(std::string_view name)
{
    if (name.empty() || name.size() > map_packs::kMaxContentName)
    {
        return fmt::format("its name is not 1-{} characters", map_packs::kMaxContentName);
    }
    if (!std::all_of(name.begin(), name.end(), printable) ||
        name.find_first_of("\\/:*?\"<>|") != std::string_view::npos)
    {
        return "its name has a character content names cannot use";
    }
    // Map packs register as rr_<id>.
    if (lower(name.substr(0, 3)) == "rr_")
    {
        return "names starting with rr_ are reserved for map packs";
    }
    return {};
}

std::vector<Decision> SelectPackages(std::span<const Candidate> candidates)
{
    std::set<std::string> files;
    std::set<std::string> tokens;
    std::set<uint32_t>    slots;
    std::set<int32_t>     ids;
    std::vector<Decision> decisions;
    for (const auto& candidate : candidates)
    {
        Decision           decision{ candidate.file, false, {} };
        const auto&        contents = candidate.contents;
        const auto         file     = lower(candidate.file);
        const auto         token    = lower(candidate.token);
        std::set<uint32_t> ownSlots;
        std::set<int32_t>  ownIds;
        // Content names are case-insensitive, so a case-sensitive folder can hold two that clash.
        if (files.contains(file))
        {
            decision.reason = "another file has the same name ignoring case";
        }
        else if (tokens.contains(token))
        {
            decision.reason = fmt::format("pack '{}' is already provided", candidate.token);
        }
        for (const auto slot : contents.slots)
        {
            if (decision.reason.empty() && (slots.contains(slot) || !ownSlots.insert(slot).second))
            {
                decision.reason = fmt::format("scenario file slot {} is already provided", slot);
            }
        }
        for (const auto& scenario : contents.scenarios)
        {
            if (decision.reason.empty() &&
                (ids.contains(scenario.id) || !ownIds.insert(scenario.id).second))
            {
                decision.reason = fmt::format("scenario ID {} is already provided", scenario.id);
            }
        }
        if (decision.reason.empty())
        {
            decision.accepted = true;
            files.insert(file);
            tokens.insert(token);
            slots.merge(ownSlots);
            ids.merge(ownIds);
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

} // namespace rerevved::retail_dlc
