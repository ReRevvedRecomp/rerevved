#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex
{

class Runtime;

}

// The lines a support request needs to diagnose a crash or hang: the build,
// the machine, the active mods and the last phase the game entered.
namespace rerevved::support_log
{

// Line formatting, shared with the tests.
namespace detail
{

struct Package
{
    std::string id;
    std::string version;
};

// "ReRevved 0.7.6 title=<commit> phase=<phase> turn=<n>", for the crash report.
inline std::string ContextText(std::string_view version, std::string_view commit, std::string_view phase, std::optional<uint32_t> turn)
{
    std::string text = "ReRevved ";
    text += version;
    text += " title=";
    text += commit;
    text += " phase=";
    text += phase.empty() ? std::string_view("startup") : phase;
    if (turn)
    {
        text += " turn=";
        text += std::to_string(*turn);
    }
    return text;
}

// "none", or "id version, id version" in load order.
inline std::string PackageList(const std::vector<Package>& packages)
{
    if (packages.empty())
        return "none";
    std::string text;
    for (const auto& package : packages)
    {
        if (!text.empty())
            text += ", ";
        text += package.id;
        if (!package.version.empty())
        {
            text += ' ';
            text += package.version;
        }
    }
    return text;
}

} // namespace detail

// The build, system and path lines; the first title lines in each log.
void LogStartup(const std::filesystem::path& userDataRoot);
// The active Lua mods and asset packages, once the loadout is built.
void LogLoadout(const rex::Runtime& runtime);
// Logs "Phase: <phase>" and makes it the crash report's phase.
void EnterPhase(std::string_view phase);
// Logs "Turn <n>" once per turn and adds it to the crash report.
void NoteTurn(uint32_t turn);

} // namespace rerevved::support_log
