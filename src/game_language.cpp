#include "game_language.h"

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(game_language, 1, "ReRevved", "Game language as an Xbox language ID: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian. Read at startup and copied to user_language, unless user_language is set explicitly")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DECLARE(uint32_t, user_language);

namespace rerevved::game_language
{

namespace
{

std::atomic<uint32_t> gActive{ kDefaultLanguage };

}

uint32_t ActiveId() noexcept
{
    return gActive.load(std::memory_order_relaxed);
}

void ApplyAtBoot()
{
    const uint32_t setting = REXCVAR_GET(game_language);
    const bool     userSet = IsExplicitSource(rex::cvar::GetFlagSource("user_language"));
    switch (DecideBoot(setting, userSet))
    {
        case BootAction::kKeepOverride:
            REXLOG_INFO("Language: user_language={} is set explicitly and overrides game_language={}", REXCVAR_GET(user_language), setting);
            break;
        case BootAction::kSkipUnsupported:
            REXLOG_WARN("Language: game_language={} is not an offered language; user_language stays {}", setting, REXCVAR_GET(user_language));
            break;
        case BootAction::kCopy:
            // Config and command line override this default; it never writes rerevved.toml.
            if (rex::cvar::SetFlagAsApplicationDefault("user_language", std::to_string(setting)))
                REXLOG_INFO("Language: {} (user_language={})", NameOf(setting), setting);
            else
                REXLOG_WARN("Language: could not set user_language={}", setting);
            break;
    }
    gActive.store(REXCVAR_GET(user_language), std::memory_order_relaxed);
}

} // namespace rerevved::game_language
