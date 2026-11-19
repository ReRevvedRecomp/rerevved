#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace rerevved::silent_player
{

// A running player's game sends at least a keep-alive every 30 s; GT2 never times one out.
inline constexpr uint64_t kSilentMs = 45000;
inline constexpr uint64_t kForgetMs = 60000;
inline constexpr size_t   kPruneAt  = 64;
// Past the online server's 60 s grace, after which a lost player cannot return to the game.
inline constexpr int32_t kDefaultTimeoutSeconds = 65;

class Tracker
{
public:
    std::optional<uint64_t> Received(uint32_t connection, uint64_t now)
    {
        auto&          player  = players_[connection];
        const bool     resumed = player.reported;
        const uint64_t gap     = now - player.heard;
        player                 = { now, now, false, false };
        return resumed ? std::optional(gap) : std::nullopt;
    }

    std::optional<uint64_t> Think(uint32_t connection, bool connected, uint64_t now)
    {
        if (players_.size() >= kPruneAt)
        {
            std::erase_if(players_, [&](const auto& entry)
                          {
                              return now - entry.second.seen >= kForgetMs;
                          });
        }
        const auto found = players_.find(connection);
        if (found == players_.end() || !connected || now - found->second.seen >= kForgetMs)
        {
            // A connection that is not yet connected, or a reused address, starts afresh.
            players_[connection] = { now, now, false, false };
            return std::nullopt;
        }
        auto& player = found->second;
        player.seen  = now;
        if (player.reported || now - player.heard < kSilentMs)
        {
            return std::nullopt;
        }
        player.reported = true;
        return now - player.heard;
    }

    std::optional<uint64_t> Expired(uint32_t connection, uint64_t now, uint64_t timeout)
    {
        const auto found = players_.find(connection);
        if (found == players_.end())
        {
            return std::nullopt;
        }
        auto& player = found->second;
        if (player.dropped || now - player.seen >= kForgetMs || now - player.heard < timeout)
        {
            return std::nullopt;
        }
        player.dropped = true;
        return now - player.heard;
    }

    size_t Size() const
    {
        return players_.size();
    }

private:
    struct Player
    {
        uint64_t heard    = 0;
        uint64_t seen     = 0;
        bool     reported = false;
        bool     dropped  = false;
    };

    std::unordered_map<uint32_t, Player> players_;
};

} // namespace rerevved::silent_player
