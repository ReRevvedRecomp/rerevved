#pragma once

// Copies the Options language to SDK user_language once at startup (restart to change).

#include <array>
#include <cstdint>
#include <string_view>

#include <rex/cvar.h>

namespace rerevved::game_language
{

struct Language
{
    uint32_t         id;
    std::string_view name;
};

// In ID order, as F1 lists them. The disc ships text for exactly these five.
inline constexpr std::array<Language, 5> kLanguages{ {
    { 1, "English" },
    { 3, "German" },
    { 4, "French" },
    { 5, "Spanish" },
    { 6, "Italian" },
} };

inline constexpr uint32_t kDefaultLanguage = 1;

// Empty for an ID the game has no text for.
constexpr std::string_view NameOf(uint32_t id) noexcept
{
    for (const auto& language : kLanguages)
    {
        if (language.id == id)
            return language.name;
    }
    return {};
}

constexpr bool IsOffered(uint32_t id) noexcept
{
    return !NameOf(id).empty();
}

enum class BootAction
{
    kCopy,
    kKeepOverride,
    kSkipUnsupported, // the setting names no offered language; leave user_language
};

constexpr BootAction DecideBoot(uint32_t setting, bool userLanguageExplicit) noexcept
{
    if (userLanguageExplicit)
        return BootAction::kKeepOverride;
    return IsOffered(setting) ? BootAction::kCopy : BootAction::kSkipUnsupported;
}

// True for config, environment or command-line writes, not defaults.
constexpr bool IsExplicitSource(rex::cvar::Source source) noexcept
{
    return source == rex::cvar::Source::kConfig || source == rex::cvar::Source::kEnvironment ||
           source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kRuntime;
}

// Call once between config load and runtime creation, before guest or XDBF reads user_language.
void ApplyAtBoot();

// Process-fixed Xbox language ID: user_language from ApplyAtBoot, else the pre-boot default.
uint32_t ActiveId() noexcept;

} // namespace rerevved::game_language
