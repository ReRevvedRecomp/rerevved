#pragma once

#include <functional>
#include <string>

namespace rerevved::online_host
{

// The F1 > Online hosting rows run on whichever thread invokes a setting, so
// both actions must only queue UI-thread work. Empty functions disable the
// rows' effect.
struct HostActions
{
    std::function<void()> quit;
    // The text is a credential: do not log it.
    std::function<void(std::string)> copy;
};

void SetHostActions(HostActions actions);

// Call it as the process ends, after the SDK has stopped hosting, so the new
// process finds the server port and the router port free.
void RelaunchIfRequested();

} // namespace rerevved::online_host
