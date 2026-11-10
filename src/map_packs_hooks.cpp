// Serve map packs as title-owned marketplace content so scans rescan scenarios without announcing a retail pack.

#include "game_content.h"
#include "map_packs.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/asset_overlay_loadout.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

REX_EXTERN(__imp__sub_82E2C890);
REX_EXTERN(__imp__sub_82E29570);
REX_EXTERN(__imp__sub_82E2A5F0);
REX_EXTERN(__imp__sub_82CC29B8);

namespace
{

namespace map_packs = rerevved::map_packs;
using rex::X_RESULT;

// Retail bits stop at 0x100 and zero masks skip rescan; this title-only bit forces rescan, hidden from DLC popup and multiplayer checks.
constexpr uint32_t kMapPackScanBit   = 0x80000000u;
constexpr uint32_t kScanOwnerMaskOff = 8;

// Parsed save header record: text pointer (length at pointer - 4) and required-content mask.
constexpr uint32_t kSaveHeaderTextOff = 4;
constexpr uint32_t kSaveHeaderMaskOff = 24;
constexpr uint32_t kMaxSaveHeaderText = 128;

std::atomic<bool> mapPacksRegistered{ false };
std::atomic<bool> saveMaskLogged{ false };
// Set for the rescan owner's own scan call only; nested owners save and restore it.
thread_local bool scanBitPending = false;

bool readArchive(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    if (ec || size > map_packs::kMaxArchiveBytes)
    {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    std::ifstream in(path, std::ios::binary);
    return in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))
        .good();
}

void stripScanBit(uint8_t* base, uint32_t owner)
{
    auto* mask = base + owner + kScanOwnerMaskOff;
    rex::memory::store_and_swap<uint32_t>(
        mask, rex::memory::load_and_swap<uint32_t>(mask) & ~kMapPackScanBit);
}

} // namespace

std::vector<rex::system::AssetOverlayPackage> rerevved::map_packs::SelectEnabled(
    const std::filesystem::path& profileRoot, const std::filesystem::path& mapsRoot)
{
    const auto catalog   = rex::system::DiscoverAssetOverlayCatalog(mapsRoot);
    const auto order     = rex::system::ReadAssetOverlayLoadout(profileRoot, {}, kOrderFileName);
    auto       selection = rex::system::SelectAssetOverlayLoadout(catalog, order);
    if (!selection.IsValid())
    {
        for (const auto& diagnostic : selection.diagnostics)
        {
            REXLOG_ERROR("Map pack selection: {}", diagnostic.message);
        }
        return {};
    }
    return std::move(selection.packages);
}

namespace
{

std::vector<std::string>                        startupOrder;
std::vector<rerevved::map_packs::ScenarioOwner> scenarioOwners;

} // namespace

const rerevved::map_packs::ScenarioOwner* rerevved::map_packs::FindScenarioOwner(int32_t scenarioId)
{
    for (const auto& owner : scenarioOwners)
    {
        if (owner.id == scenarioId)
        {
            return &owner;
        }
    }
    return nullptr;
}

const std::vector<std::string>& rerevved::map_packs::StartupOrder()
{
    return startupOrder;
}

void rerevved::map_packs::RegisterSelected(
    std::span<const rex::system::AssetOverlayPackage> packages)
{
    startupOrder.clear();
    scenarioOwners.clear();
    for (const auto& package : packages)
        startupOrder.push_back(package.id);
    std::vector<Candidate>             candidates;
    std::vector<std::filesystem::path> roots;
    for (const auto& package : packages)
    {
        const auto root    = package.assets_root / kPackDirectory;
        const auto archive = root / kArchiveName;
        if (auto error = CheckPackFiles(package.assets_root); !error.empty())
        {
            REXLOG_ERROR("Map pack '{}' refused: {}", package.id, error);
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(archive, ec))
        {
            REXLOG_ERROR("Map pack '{}' refused: assets/map-pack/dlc.fpk is missing", package.id);
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!readArchive(archive, bytes))
        {
            REXLOG_ERROR("Map pack '{}' refused: dlc.fpk is unreadable or larger than {} bytes",
                         package.id,
                         kMaxArchiveBytes);
            continue;
        }
        if (ContentName(package.id).empty())
        {
            REXLOG_ERROR("Map pack '{}' refused: package ID cannot name content", package.id);
            continue;
        }
        Candidate candidate{ package.id, {} };
        if (auto error = ParseArchive(bytes, candidate.contents); !error.empty())
        {
            REXLOG_ERROR("Map pack '{}' refused: {}", package.id, error);
            continue;
        }
        candidates.push_back(std::move(candidate));
        roots.push_back(root);
    }
    if (candidates.empty())
    {
        return;
    }

    auto*      content   = REX_KERNEL_STATE()->content_manager();
    const auto decisions = SelectPacks(candidates);
    for (size_t index = 0; index < decisions.size(); ++index)
    {
        const auto& decision = decisions[index];
        if (!decision.accepted)
        {
            REXLOG_ERROR("Map pack '{}' refused: {}", decision.id, decision.reason);
            continue;
        }
        const auto result =
            content->AddTitlePackage(rerevved::kTitleId, ContentName(decision.id), roots[index]);
        if (result != X_ERROR_SUCCESS)
        {
            REXLOG_ERROR("Map pack '{}' refused: content registration failed ({:08X})",
                         decision.id,
                         result);
            continue;
        }
        mapPacksRegistered.store(true);
        for (const auto& scenario : candidates[index].contents.scenarios)
        {
            scenarioOwners.push_back({ scenario.id, decision.id, scenario.map });
        }
        REXLOG_INFO("Map pack '{}' registered with {} scenario(s)", decision.id, candidates[index].contents.scenarios.size());
    }
}

REX_HOOK_RAW(sub_82E2C890)
{
    const uint32_t owner = ctx.r3.u32;
    const bool     outer = scanBitPending;
    stripScanBit(base, owner);
    scanBitPending = mapPacksRegistered.load();
    __imp__sub_82E2C890(ctx, base);
    scanBitPending = outer;
    stripScanBit(base, owner);
}

REX_HOOK_RAW(sub_82E29570)
{
    const bool addBit = std::exchange(scanBitPending, false);
    __imp__sub_82E29570(ctx, base);
    if (addBit)
    {
        ctx.r3.u64 = ctx.r3.u32 | kMapPackScanBit;
    }
}

REX_HOOK_RAW(sub_82E2A5F0)
{
    __imp__sub_82E2A5F0(ctx, base);
    ctx.r3.u64 = ctx.r3.u32 & ~kMapPackScanBit;
}

// A save on a pack scenario records a scenario pack bit that no installed content supplies, so the Load list refuses it.
REX_HOOK_RAW(sub_82CC29B8)
{
    const uint32_t record = ctx.r6.u32;
    __imp__sub_82CC29B8(ctx, base);
    if (!record || !ctx.r3.u32)
    {
        return;
    }
    auto*          maskAt = base + record + kSaveHeaderMaskOff;
    const uint32_t mask   = rex::memory::load_and_swap<uint32_t>(maskAt);
    const uint32_t text   = rex::memory::load_and_swap<uint32_t>(base + record + kSaveHeaderTextOff);
    if (!(mask & ~map_packs::kSaveWonderPackBits) || text < 4)
    {
        return;
    }
    const uint32_t length = rex::memory::load_and_swap<uint32_t>(base + text - 4);
    if (!length || length > kMaxSaveHeaderText)
    {
        return;
    }
    const auto  scenario = map_packs::SaveScenarioId({ reinterpret_cast<const char*>(base + text), length });
    const auto* owner    = scenario ? map_packs::FindScenarioOwner(*scenario) : nullptr;
    if (!owner)
    {
        return;
    }
    rex::memory::store_and_swap<uint32_t>(maskAt, mask & map_packs::kSaveWonderPackBits);
    if (!saveMaskLogged.exchange(true))
    {
        REXLOG_INFO("Map pack '{}' supplies scenario {}: save content mask {:08X} accepted", owner->pack, *scenario, mask);
    }
}
