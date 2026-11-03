#include "keybind_defaults.h"

#include <string>

#include <rex/cvar.h>

namespace rerevved::keybind_defaults
{

size_t Apply(std::span<const Default> defaults)
{
    size_t applied = 0;
    for (const auto& entry : defaults)
    {
        if (rex::cvar::SetFlagAsApplicationDefault(entry.key, entry.value))
        {
            ++applied;
            continue;
        }
        // CLI/environment values outrank SDK application defaults; keep their source, but reset restores the title's default.
        const auto source = rex::cvar::GetFlagSource(entry.key);
        if (source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment)
            continue;
        for (auto& flag : rex::cvar::GetRegistry())
        {
            if (flag.name == entry.key)
            {
                flag.default_value = std::string(entry.value);
                ++applied;
                break;
            }
        }
    }
    return applied;
}

} // namespace rerevved::keybind_defaults
