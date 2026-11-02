#include "map_packs.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <set>
#include <system_error>

#include <fmt/format.h>

namespace rerevved::map_packs
{

namespace
{

constexpr uint32_t         kFpkVersion   = 6;
constexpr size_t           kHeaderSize   = 14;
constexpr size_t           kRecordTail   = 16;
constexpr std::string_view kScenarioStem = "DLCScenarioData";
constexpr std::string_view kScenarioExt  = ".xml";

uint32_t readU32(std::span<const uint8_t> bytes, size_t offset)
{
    const uint8_t* p = bytes.data() + offset;
    return uint32_t{ p[0] } | (uint32_t{ p[1] } << 8) | (uint32_t{ p[2] } << 16) |
           (uint32_t{ p[3] } << 24);
}

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return result;
}

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool elementText(std::string_view body, std::string_view tag, std::string_view& text)
{
    const std::string open  = fmt::format("<{}>", tag);
    const std::string close = fmt::format("</{}>", tag);
    const auto        begin = body.find(open);
    if (begin == std::string_view::npos)
    {
        return false;
    }
    const auto start = begin + open.size();
    const auto end   = body.find(close, start);
    if (end == std::string_view::npos)
    {
        return false;
    }
    text = trim(body.substr(start, end - start));
    return true;
}

// DLCScenarioDataN.xml is the only accepted filename form.
bool scenarioSlot(std::string_view name, uint32_t& slot)
{
    if (name.size() <= kScenarioStem.size() + kScenarioExt.size() ||
        lower(name.substr(0, kScenarioStem.size())) != lower(kScenarioStem) ||
        lower(name.substr(name.size() - kScenarioExt.size())) != kScenarioExt)
    {
        return false;
    }
    const auto digits = name.substr(kScenarioStem.size(),
                                    name.size() - kScenarioStem.size() - kScenarioExt.size());
    // Leading zeros would give one slot two archive names.
    if (digits.size() > 1 && digits.front() == '0')
    {
        return false;
    }
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), slot);
    return error == std::errc{} && end == digits.data() + digits.size();
}

std::string withoutComments(std::string_view xml)
{
    std::string result;
    size_t      cursor = 0;
    while (cursor < xml.size())
    {
        const auto begin = xml.find("<!--", cursor);
        result.append(xml.substr(cursor, begin - cursor));
        if (begin == std::string_view::npos)
        {
            break;
        }
        const auto end = xml.find("-->", begin + 4);
        cursor         = end == std::string_view::npos ? xml.size() : end + 3;
    }
    return result;
}

std::string parseScenarios(std::string_view source, std::string_view entry, Contents& contents)
{
    // Tools and retail packs write bare EntryInfo elements with ID and MAP
    // children.
    const std::string          text = withoutComments(source);
    const std::string_view     xml(text);
    constexpr std::string_view kOpen  = "<EntryInfo>";
    constexpr std::string_view kClose = "</EntryInfo>";
    size_t                     cursor = 0;
    size_t                     found  = 0;
    while ((cursor = xml.find(kOpen, cursor)) != std::string_view::npos)
    {
        const auto start = cursor + kOpen.size();
        const auto close = xml.find(kClose, start);
        if (close == std::string_view::npos || xml.substr(start, close - start).find(kOpen) !=
                                                   std::string_view::npos)
        {
            return fmt::format("{} has an unterminated EntryInfo", entry);
        }
        const auto       body = xml.substr(start, close - start);
        std::string_view idText;
        std::string_view map;
        Scenario         scenario;
        if (!elementText(body, "ID", idText) || !elementText(body, "MAP", map) || map.empty())
        {
            return fmt::format("{} has an entry without ID or MAP", entry);
        }
        const auto [end, error] =
            std::from_chars(idText.data(), idText.data() + idText.size(), scenario.id);
        if (error != std::errc{} || end != idText.data() + idText.size())
        {
            return fmt::format("{} has a non-numeric ID '{}'", entry, idText);
        }
        if (scenario.id < kFirstScenarioId)
        {
            return fmt::format("{} uses reserved scenario ID {}", entry, scenario.id);
        }
        scenario.map = std::string(map);
        contents.scenarios.push_back(std::move(scenario));
        cursor = close;
        ++found;
    }
    return found == 0 ? fmt::format("{} has no scenarios", entry) : std::string{};
}

} // namespace

std::string ParseArchive(std::span<const uint8_t> archive, Contents& contents)
{
    contents = {};
    if (archive.size() < kHeaderSize || readU32(archive, 0) != kFpkVersion ||
        std::memcmp(archive.data() + 4, "FPK_", 4) != 0)
    {
        return "dlc.fpk is not a version 6 FPK archive";
    }

    const uint32_t count  = readU32(archive, 10);
    size_t         offset = kHeaderSize;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (archive.size() - offset < 4)
        {
            return "dlc.fpk table is truncated";
        }
        const uint32_t nameLength = readU32(archive, offset);
        const size_t   padded     = (size_t{ nameLength } + 3) & ~size_t{ 3 };
        if (nameLength == 0 || archive.size() - offset - 4 < padded + kRecordTail)
        {
            return "dlc.fpk table is truncated";
        }
        const std::string_view name(reinterpret_cast<const char*>(archive.data() + offset + 4),
                                    nameLength);
        offset += 4 + padded;
        const uint32_t size       = readU32(archive, offset + 8);
        const uint32_t dataOffset = readU32(archive, offset + 12);
        offset += kRecordTail;
        if (dataOffset > archive.size() || size > archive.size() - dataOffset)
        {
            return fmt::format("dlc.fpk entry {} lies outside the archive", name);
        }
        contents.entryNames.emplace_back(name);

        uint32_t slot = 0;
        if (!scenarioSlot(name, slot))
        {
            continue;
        }
        if (slot < kFirstPackSlot || slot > kLastPackSlot)
        {
            return fmt::format("{} uses reserved scenario file slot {}", name, slot);
        }
        contents.slots.push_back(slot);
        const std::string_view xml(reinterpret_cast<const char*>(archive.data() + dataOffset), size);
        if (auto error = parseScenarios(xml, name, contents); !error.empty())
        {
            return error;
        }
    }
    return contents.slots.empty() ? "dlc.fpk has no DLCScenarioData5-31.xml" : std::string{};
}

std::vector<Decision> SelectPacks(std::span<const Candidate> candidates)
{
    std::set<std::string> entries;
    std::set<std::string> maps;
    std::set<int32_t>     ids;
    std::vector<Decision> decisions;
    for (const auto& candidate : candidates)
    {
        Decision              decision{ candidate.id, false, {} };
        const auto&           contents = candidate.contents;
        std::set<std::string> ownEntries;
        std::set<std::string> ownMaps;
        std::set<int32_t>     ownIds;
        for (const auto& name : contents.entryNames)
        {
            const auto key = lower(name);
            if (entries.contains(key) || !ownEntries.insert(key).second)
            {
                decision.reason = fmt::format("archive entry {} is already provided", name);
                break;
            }
        }
        for (const auto& scenario : contents.scenarios)
        {
            if (!decision.reason.empty())
            {
                break;
            }
            const auto key = lower(scenario.map);
            if (maps.contains(key) || !ownMaps.insert(key).second)
            {
                decision.reason = fmt::format("MAP {} is already provided", scenario.map);
            }
            else if (ids.contains(scenario.id) || !ownIds.insert(scenario.id).second)
            {
                decision.reason = fmt::format("scenario ID {} is already provided", scenario.id);
            }
        }
        if (decision.reason.empty())
        {
            decision.accepted = true;
            entries.merge(ownEntries);
            maps.merge(ownMaps);
            ids.merge(ownIds);
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

std::string ContentName(std::string_view packId)
{
    auto name = fmt::format("rr_{}", packId);
    if (packId.empty() || name.size() > kMaxContentName ||
        packId.find_first_of("\\/:") != std::string_view::npos)
    {
        return {};
    }
    return name;
}

std::string CheckPackFiles(const std::filesystem::path& assetsRoot)
{
    const auto      archive = std::filesystem::path(kPackDirectory) / kArchiveName;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(assetsRoot, ec), end; !ec && it != end;
         it.increment(ec))
    {
        if (it->is_directory(ec))
        {
            continue;
        }
        const auto relative = it->path().lexically_relative(assetsRoot);
        if (relative != archive)
        {
            return fmt::format("assets/{} is not part of a map pack", relative.generic_string());
        }
    }
    if (ec)
    {
        return "its assets folder is unreadable";
    }
    return {};
}

} // namespace rerevved::map_packs
