#include "achievements_list.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace rerevved::achievements_list
{

namespace
{

constexpr uint64_t kFileTimePerSecond = 10000000ull;
constexpr uint64_t kUnixEpochFileTime = 116444736000000000ull;

} // namespace

std::vector<Entry> Build(std::span<const rex::system::AchievementInfo> infos,
                         const IsUnlockedFn&                           isUnlocked,
                         const UnlockTimeFn&                           unlockTime)
{
    std::vector<Entry> original;
    std::vector<Entry> custom;
    for (const auto& info : infos)
    {
        Entry entry;
        entry.id          = info.id;
        entry.label       = info.label;
        entry.gamerscore  = info.gamerscore;
        entry.iconPath    = info.icon_path;
        entry.imageId     = info.image_id;
        entry.custom      = info.custom;
        entry.unlocked    = isUnlocked && isUnlocked(info.id);
        entry.description = entry.unlocked || info.unachieved_description.empty() ? info.description : info.unachieved_description;
        if (entry.unlocked)
            entry.unlockedAt = unlockTime ? unlockTime(info.id) : 0;
        (info.custom ? custom : original).push_back(std::move(entry));
    }
    std::stable_sort(custom.begin(), custom.end(), [](const Entry& a, const Entry& b)
                     {
                         if (a.gamerscore != b.gamerscore)
                             return a.gamerscore < b.gamerscore;
                         if (a.label != b.label)
                             return a.label < b.label;
                         return a.id < b.id;
                     });
    original.insert(original.end(), std::make_move_iterator(custom.begin()), std::make_move_iterator(custom.end()));
    return original;
}

Groups Split(std::span<const Entry> entries)
{
    Groups groups;
    for (const auto& entry : entries)
        (entry.custom ? groups.custom : groups.original).push_back(entry);
    return groups;
}

Totals Summarize(std::span<const Entry> entries)
{
    Totals totals;
    totals.total = static_cast<int>(entries.size());
    for (const auto& entry : entries)
    {
        totals.possible += entry.gamerscore;
        if (entry.unlocked)
        {
            ++totals.unlocked;
            totals.earned += entry.gamerscore;
        }
    }
    return totals;
}

std::string Substitute(std::string_view text, std::span<const std::string> args)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '{')
        {
            size_t j     = i + 1;
            size_t index = 0;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9' && index < 1000)
                index = index * 10 + static_cast<size_t>(text[j++] - '0');
            if (j > i + 1 && j < text.size() && text[j] == '}' && index < args.size())
            {
                out += args[index];
                i = j + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

std::optional<int64_t> UnixSeconds(uint64_t fileTime)
{
    if (fileTime < kUnixEpochFileTime)
        return std::nullopt;
    return static_cast<int64_t>((fileTime - kUnixEpochFileTime) / kFileTimePerSecond);
}

std::string FormatDate(int year, int month, int day)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
    return buffer;
}

} // namespace rerevved::achievements_list
