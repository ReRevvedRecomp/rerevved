#pragma once

#include "host_settings.h"

#include <optional>
#include <vector>

#include <rex/session_data.h>

namespace rerevved::lobby_content_watch
{

using LocalProvider = host_settings::HostSettings (*)();
using Poster        = void (*)(const host_settings::LobbyWarning&);

class Watch
{
public:
    void Poll(const rex::session_data::Status& status, LocalProvider local, Poster post);

private:
    std::optional<std::vector<uint8_t>> seen_;
};

// The host's countdown needs every player ready, so a refused press keeps that game from starting.
std::optional<host_settings::LobbyWarning> ReadyRefusal(const rex::session_data::Status& status, LocalProvider local);

// The SDK's session data has no arrival callback, so a background poll watches it.
void Start();
void Stop();

} // namespace rerevved::lobby_content_watch

bool RefuseLobbyReady();
