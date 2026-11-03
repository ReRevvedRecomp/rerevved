#include "achievement_icons.h"

#include <memory>
#include <utility>

#include <rex/logging.h>

namespace rerevved::achievement_icons
{
namespace
{

constexpr std::string_view kPrefix = "fpk:";

bool isName(std::string_view name)
{
    return !name.empty() && name.find_first_of("/\\:") == std::string_view::npos;
}

} // namespace

std::optional<Source> ParseIconPath(std::string_view iconPath)
{
    if (!iconPath.starts_with(kPrefix))
        return std::nullopt;
    iconPath.remove_prefix(kPrefix.size());
    const size_t slash = iconPath.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const std::string_view archive = iconPath.substr(0, slash);
    const std::string_view entry   = iconPath.substr(slash + 1);
    if (!isName(archive) || !isName(entry))
        return std::nullopt;
    return Source{ std::string(archive), std::string(entry) };
}

ReadFn GameReader(std::filesystem::path gameRoot, std::vector<Source> wanted)
{
    struct Entry
    {
        std::optional<std::vector<uint8_t>> bytes;
        std::string                         error;
    };

    auto archives = std::make_shared<std::unordered_map<std::string, std::unordered_map<std::string, Entry>>>();
    return [root = std::move(gameRoot), wanted = std::move(wanted), archives](const Source& source) -> std::optional<retail_ui_art::Image>
    {
        auto [archive, first] = archives->try_emplace(source.archive);
        if (first)
        {
            std::vector<std::string_view> names{ source.entry };
            for (const auto& w : wanted)
            {
                if (w.archive == source.archive && w.entry != source.entry)
                    names.push_back(w.entry);
            }
            std::vector<std::optional<std::vector<uint8_t>>> bytes;
            std::vector<std::string>                         errors;
            std::string                                      error;
            const bool                                       ok = retail_ui_art::ReadFpkEntries(root / "Resource" / "Common" / source.archive, names, bytes, errors, &error);
            for (size_t i = 0; i < names.size(); ++i)
                archive->second[std::string(names[i])] = ok ? Entry{ std::move(bytes[i]), errors[i] } : Entry{ std::nullopt, error };
        }
        auto&       entry = archive->second[source.entry];
        std::string error = entry.error;
        if (!entry.bytes)
        {
            REXLOG_WARN("Achievement icon: {}:{} unavailable ({})", source.archive, source.entry, error);
            return std::nullopt;
        }
        auto image = retail_ui_art::DecodeDds(*entry.bytes, &error);
        if (!image)
            REXLOG_WARN("Achievement icon: {}:{} not decoded ({})", source.archive, source.entry, error);
        return image;
    };
}

Provider::Provider(ReadFn read)
: read_(std::move(read))
{
}

rex::ui::AchievementIconPixels Provider::operator()(const rex::system::AchievementInfo& info)
{
    const auto source = ParseIconPath(info.icon_path);
    if (!source)
        return {};
    auto found = images_.find(info.icon_path);
    if (found == images_.end())
        found = images_.emplace(info.icon_path, read_(*source)).first;
    if (!found->second)
        return {};
    return { found->second->width, found->second->height, found->second->rgba };
}

} // namespace rerevved::achievement_icons
