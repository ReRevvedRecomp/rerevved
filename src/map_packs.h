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

// Packs live beside the executable (asset-pack.toml, assets/map-pack/dlc.fpk); map_order.txt orders them.
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

// Returns an empty string on success, otherwise why the pack is refused.
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

// A pack repeating an earlier accepted pack's archive entries, MAP names or scenario IDs is refused.
std::vector<Decision> SelectPacks(std::span<const Candidate> candidates);

// XContent file names hold at most 42 characters.
inline constexpr size_t kMaxContentName = 42;

std::string ContentName(std::string_view packId);

// Empty when the assets folder holds only map-pack/dlc.fpk, otherwise why it is refused.
std::string CheckPackFiles(const std::filesystem::path& assetsRoot);

// Problems are logged; an invalid order file enables none.
std::vector<rex::system::AssetOverlayPackage> SelectEnabled(
    const std::filesystem::path& profileRoot, const std::filesystem::path& mapsRoot);

// Registers the map pack of each selected package, in loadout order, before
// the guest main thread starts. Refused packs are logged by package ID.
void RegisterSelected(std::span<const rex::system::AssetOverlayPackage> packages);

} // namespace rerevved::map_packs
