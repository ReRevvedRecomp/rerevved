#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gfx_movie_rows
{

enum class Movie
{
    WinLoseScreen,
    Histograph,
};

// GFx keeps the retail movies it loaded at startup, so widened games ask for
// the patched copies under names it has never loaded.
inline constexpr std::string_view kRetailArchive      = "Common0.FPK";
inline constexpr std::string_view kWinLoseEntry       = "gfx_winlosescreen.gfx";
inline constexpr std::string_view kHistographEntry    = "gfx_histograph.gfx";
inline constexpr std::string_view kWinLoseRowsName    = "GFX_WinLoseScreenRows.gfx";
inline constexpr std::string_view kHistographRowsName = "GFX_HistographRows.gfx";

// Rows the patched movies hold: win/lose place clips port1..port6 (port0 is the
// winner) and Histograph legend rows playerInfo1..playerInfo8.
inline constexpr uint32_t kWinLosePlaceRows = 6;
inline constexpr uint32_t kHistographRows   = 8;

// Retail movies that draw their button icons as shapes instead of loading images.
enum class PromptMovie
{
    EndGameCrawl,
    StatScreen,
    WaitingOverlay,
    GpDirectory,
};

inline constexpr size_t           kPromptMovies     = 4;
inline constexpr std::string_view kPromptDonorEntry = "gfx_pauseoverlay.gfx";

struct Result
{
    std::vector<uint8_t> bytes;  // patched movie; empty unless the patch applied
    std::string_view     reason; // why the source was left alone; empty on success
};

std::string_view RowsMovieName(Movie movie);

// Matches a guest file name against the patched movie names, ignoring case.
std::optional<Movie> FindRowsMovie(std::string_view fileName);

// Pure byte transform on a player's own retail movie. The source must match the
// retail structure exactly; anything else fails and nothing is derived from it.
Result PatchMovie(Movie movie, std::span<const uint8_t> source);

std::string_view PromptMovieName(PromptMovie movie);

// Ignores case and path.
std::optional<PromptMovie> FindPromptMovie(std::string_view fileName);

// Makes the target's button frames load images, using sprites copied from the donor.
// Fails when either movie differs from the retail structure.
Result PatchPromptMovie(std::span<const uint8_t> target, std::span<const uint8_t> donor);

struct Prepared
{
    std::vector<uint8_t> bytes;  // patched movie; empty when unavailable
    std::string          reason; // why it is unavailable
};

// Set before the guest runs; the root holds Resource/Common/Common0.FPK.
void SetGameRoot(std::filesystem::path root);

// Reads and patches the player's retail movie once per process.
const Prepared& PreparedMovie(Movie movie);

const Prepared& PreparedPromptMovie(PromptMovie movie);

// The patched win/lose movie shows places 6 and 7 with these images, drawn
// once per process from the player's place 5 badge.
const Prepared& PreparedBadge(int place);

} // namespace rerevved::gfx_movie_rows
