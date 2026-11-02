#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <filesystem>

#include <rex/system/asset_overlay_catalog.h>

namespace rerevved::map_packs
{

// Map packs live beside the executable with an asset-pack.toml manifest and one
// DLC archive at assets/map-pack/dlc.fpk. The profile's map_order.txt
// orders/enables them independently of asset overrides.
inline constexpr std::string_view kMapsDirectory   = "maps";
inline constexpr std::string_view kOrderFileName   = "map_order.txt";
inline constexpr std::string_view kPackDirectory   = "map-pack";
inline constexpr std::string_view kArchiveName     = "dlc.fpk";
inline constexpr size_t           kMaxArchiveBytes = 256u * 1024u * 1024u;

// Retail packs use scenario files 0-4 and scenario IDs below 100.
inline constexpr uint32_t kFirstPackSlot   = 5;
inline constexpr uint32_t kLastPackSlot    = 31;
inline constexpr int32_t  kFirstScenarioId = 100;

struct Scenario
{
    int32_t     id = 0;
    std::string map;
};

struct Contents
{
    std::vector<std::string> entryNames;
    std::vector<uint32_t>    slots;
    std::vector<Scenario>    scenarios;
};

// Reads the archive table and every DLCScenarioDataN.xml it holds. Returns an
// empty string on success, otherwise why the pack is refused.
std::string ParseArchive(std::span<const uint8_t> archive, Contents& contents);

struct Candidate
{
    std::string id;
    Contents    contents;
};

struct Decision
{
    std::string id;
    bool        accepted = false;
    std::string reason;
};

// Accepts candidates in order; a pack whose archive entries, MAP names or
// scenario IDs repeat an earlier accepted pack is refused.
std::vector<Decision> SelectPacks(std::span<const Candidate> candidates);

// XContent file names hold at most 42 characters.
inline constexpr size_t kMaxContentName = 42;

// The XContent file name a pack is served under, or empty when the package ID
// cannot form one.
std::string ContentName(std::string_view packId);

// Empty when an assets folder holds only map-pack/dlc.fpk, otherwise why the
// pack is refused.
std::string CheckPackFiles(const std::filesystem::path& assetsRoot);

// The packs of mapsRoot enabled by profileRoot's map_order.txt, in order.
// Problems are logged; an invalid order file enables none.
std::vector<rex::system::AssetOverlayPackage> SelectEnabled(
    const std::filesystem::path& profileRoot, const std::filesystem::path& mapsRoot);

// Registers the map pack of each selected package, in loadout order, before
// the guest main thread starts. Refused packs are logged by package ID.
void RegisterSelected(std::span<const rex::system::AssetOverlayPackage> packages);

} // namespace rerevved::map_packs
