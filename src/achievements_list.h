#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/achievement_store.h>

namespace rerevved::achievements_list
{

struct Entry
{
    uint32_t    id = 0;
    std::string label;
    std::string description;
    uint32_t    gamerscore = 0;
    // SDK icon: metadata path, falling back to a title image ID.
    std::string iconPath;
    uint32_t    imageId  = 0;
    bool        custom   = false;
    bool        unlocked = false;
    // Windows FILETIME of the unlock; 0 when unknown or locked.
    uint64_t unlockedAt = 0;
};

struct Totals
{
    int      unlocked = 0;
    int      total    = 0;
    uint32_t earned   = 0;
    uint32_t possible = 0;
};

using IsUnlockedFn = std::function<bool(uint32_t)>;
using UnlockTimeFn = std::function<uint64_t(uint32_t)>;

// F3 order: originals in catalog order, then custom by ascending gamerscore, label and ID.
std::vector<Entry> Build(std::span<const rex::system::AchievementInfo> infos,
                         const IsUnlockedFn&                           isUnlocked,
                         const UnlockTimeFn&                           unlockTime);

// Tabs exist only with custom achievements.
struct Groups
{
    std::vector<Entry> original;
    std::vector<Entry> custom;

    bool HasTabs() const
    {
        return !custom.empty();
    }

    // The entries of a tab; any tab but 1 is the original group.
    const std::vector<Entry>& Tab(size_t tab) const
    {
        return tab == 1 ? custom : original;
    }
};

Groups Split(std::span<const Entry> entries);

Totals Summarize(std::span<const Entry> entries);

// Replaces {0}, {1}, ... with the arguments; an index without an argument is left as written.
std::string Substitute(std::string_view text, std::span<const std::string> args);

// Seconds since 1970 UTC for a FILETIME; none for 0 or a time before 1970.
std::optional<int64_t> UnixSeconds(uint64_t fileTime);

std::string FormatDate(int year, int month, int day);

} // namespace rerevved::achievements_list
