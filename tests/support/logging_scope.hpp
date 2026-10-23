#pragma once

// Starts the SDK logger for one test case. The SDK cannot start again after
// ShutdownLogging (it empties the category registry but keeps its initialized
// flag), so the logger stays up until the process exits and later cases, which
// may assert on log output, still find their loggers.

#include <rex/logging.h>

namespace rerevved::tests
{

class ScopedLogging
{
public:
    explicit ScopedLogging(bool toConsole = true)
    {
        rex::LogConfig config;
        config.log_to_console = toConsole;
        rex::InitLogging(config);
    }

    ~ScopedLogging() = default;

    ScopedLogging(const ScopedLogging&)            = delete;
    ScopedLogging& operator=(const ScopedLogging&) = delete;
};

} // namespace rerevved::tests
