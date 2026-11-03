#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/system/achievement_store.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

#include "retail_ui_art.h"

namespace rerevved::achievement_icons
{

// An icon_path of "fpk:<archive>/<entry>" names a DDS in an FPK under Resource/Common.
struct Source
{
    std::string archive;
    std::string entry;
};

std::optional<Source> ParseIconPath(std::string_view iconPath);

using ReadFn = std::function<std::optional<retail_ui_art::Image>(const Source&)>;

// Batches all wanted entries on an archive's first use so the UI thread
// reads each archive only once.
ReadFn GameReader(std::filesystem::path gameRoot, std::vector<Source> wanted);

// Decodes each named picture once; a failure is remembered and yields no pixels.
class Provider
{
public:
    explicit Provider(ReadFn read);

    rex::ui::AchievementIconPixels operator()(const rex::system::AchievementInfo& info);

private:
    ReadFn                                                               read_;
    std::unordered_map<std::string, std::optional<retail_ui_art::Image>> images_;
};

} // namespace rerevved::achievement_icons
