#include "save_sidecar.h"

#include "custom_achievements.h"
#include "gameplay_fixes.h"
#include "gotw_hooks.h"
#include "gotw_practice.h"
#include "mod_script_rules.h"
#include "occ_settings.h"
#include "raging_barbarians_settings.h"
#include "start_resources_runtime.h"

#include <stdexcept>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(save_sidecar, true, "ReRevved", "Persist per-save Resources, Raging Barbarians, One City Challenge and AI policies in a host sidecar");

REX_EXTERN(__imp__sub_82D82510);
REX_EXTERN(__imp__sub_82D84238);
REX_EXTERN(__imp__sub_82D387E0);

namespace
{

namespace sidecar = rerevved::save_sidecar;

struct Transaction;
thread_local Transaction* active = nullptr;

struct Transaction
{
    Transaction(uint32_t selectedRecord, bool loading, bool manualSave = false)
    : previous(active)
    , record(selectedRecord)
    , load(loading)
    , manual(manualSave)
    , enabled(REXCVAR_GET(save_sidecar))
    , forced(!loading && !enabled && rerevved::gotw::ForGame().Isolates())
    {
        active = this;
    }

    ~Transaction()
    {
        active = previous;
        if (load)
        {
            rerevved::gotw::ArmLoadIsolation(false);
        }
        try
        {
            if (enabled && load && !restoreLogged)
            {
                if (pending.AfterReset())
                {
                    REXLOG_WARN("Save sidecar: {}", restoreFailed ? "restore failed" : "staged state never restored");
                }
                else
                {
                    REXLOG_INFO("Save sidecar: {}; no saved gameplay settings restored", nativeFile.empty() ? "no native file captured" : fallbackReason);
                }
            }
        }
        catch (...)
        {
        }
    }

    Transaction* previous;
    uint32_t     record;
    bool         load;
    bool         manual;
    bool         enabled; // save_sidecar: store/restore ordinary policy.
    // Practice/isolated saves need their block or marker even with save_sidecar off.
    bool                  forced;
    std::filesystem::path nativeFile;
    sidecar::PendingLoad  pending;
    const char*           fallbackReason   = "sidecar read not reached";
    bool                  restoreLogged    = false;
    bool                  restoreFailed    = false;
    bool                  practiceVerified = false;
    // Schema 3 or later, even if rejected or mismatched.
    bool weeklyClaim = false;
};

const uint8_t* checkedBytes(uint64_t address, uint32_t size)
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || !size || address < 0x10000 || address + size > 0xFFFFF000ull)
    {
        throw std::runtime_error("Save sidecar: unavailable guest memory");
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + size - 1;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap ||
        (static_cast<uint32_t>(heap->QueryRangeAccess(first, last)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) == 0)
    {
        throw std::runtime_error("Save sidecar: inaccessible guest range");
    }
    return memory->TranslateVirtual<const uint8_t*>(first);
}

uint32_t readWord(uint64_t address)
{
    const auto* bytes = checkedBytes(address, 4);
    return uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
}

std::string readString(uint64_t address, size_t limit)
{
    std::string value;
    for (size_t i = 0; i < limit; ++i)
    {
        const char byte = static_cast<char>(*checkedBytes(address + i, 1));
        if (!byte)
        {
            return value;
        }
        value.push_back(byte);
    }
    throw std::runtime_error("Save sidecar: unterminated guest filename");
}

// Only a manual practice save with a policy carries a practice block; others get the isolation marker.
std::optional<rerevved::gotw::PracticeBlock> practiceBlock(bool manual)
{
    if (!manual || !rerevved::gotw::ForGame().practice)
    {
        return std::nullopt;
    }
    try
    {
        const auto    civilization = readWord(uint64_t{ readWord(0x8314EFC8) } + 0x13C);
        const int32_t restored     = rerevved::gameplay_fixes::RestoredDominationMinimum();
        const int32_t minimum      = restored ? restored
                                              : rerevved::gameplay_fixes::DominationMinimumTurns(rex::cvar::Query<std::string>("domination_min_turns"), rerevved::gameplay_fixes::ShortenedMinimumAllowed());
        return rerevved::gotw::CurrentPracticeBlock(civilization, minimum);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

sidecar::State captureState(bool manual)
{
    const auto occ      = rerevved::occ::ForGame();
    const auto progress = rerevved::custom_achievements::CurrentProgress();
    auto       block    = practiceBlock(manual);
    const bool marker   = rerevved::gotw::NeedsIsolationMarker(block.has_value());
    return { rerevved::raging_barbarians::Evaluate(0).enabled,
             occ->enabled,
             occ->minimumPopulation,
             rerevved::start_resources::ForGame(),
             *rerevved::resource_identity_overrides::Current(),
             rerevved::ai_economy::ForSave(rerevved::ai_economy::NativeCostsRequired()),
             progress.occFromSetup,
             progress.barbarianDefeats,
             progress.historyKnown,
             progress.unitBuilt,
             progress.battleFought,
             progress.deepOceanEntered,
             progress.firstFoundingTurn,
             progress.initialAiOpponents,
             std::move(block),
             marker,
             rerevved::gameplay_fixes::TurnLimitForGame(),
             rerevved::gameplay_fixes::AlwaysAtWarForGame(),
             progress.navalVictories };
}

} // namespace

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/docs/reference/save-storage.md#sidecar-intervention-edges
// The wrapper bounds the pending restore to this native load, including failures.
REX_HOOK_RAW(sub_82D84238)
{
    Transaction                     transaction(ctx.r4.u32, true);
    rerevved::ai_economy::LoadScope economyLoad;
    __imp__sub_82D84238(ctx, base);
    economyLoad.Finish(rerevved::ai_economy::NativeCostsRequired());
}

REX_HOOK_RAW(sub_82D82510)
{
    // Manual Save is 0x82D00550 (return 0x82D005C4); the automatic wrapper 0x82D83E80 stays native.
    Transaction transaction(ctx.r4.u32, false, ctx.lr == 0x82D005C4);
    __imp__sub_82D82510(ctx, base);
}

REX_HOOK_RAW(sub_82D387E0)
{
    // The selected-content stream calls this opener at 0x82E83B38.
    // Its +0x10 string holds the normalized native guest filename on return.
    const bool     capture = active && (active->enabled || active->forced || active->load) && ctx.lr == 0x82E83B3C;
    const uint32_t stream  = ctx.r3.u32;
    if (capture)
    {
        active->nativeFile.clear();
    }
    __imp__sub_82D387E0(ctx, base);
    if (!capture || ctx.r3.u32 != 0)
    {
        return;
    }
    try
    {
        const auto filename = readString(readWord(uint64_t{ stream } + 0x10), 1024);
        auto*      fs       = REX_KERNEL_STATE()->file_system();
        auto*      entry    = fs ? dynamic_cast<rex::filesystem::HostPathEntry*>(fs->ResolvePath(filename)) : nullptr;
        if (entry)
        {
            active->nativeFile = entry->host_path();
        }
    }
    catch (...)
    {
        active->nativeFile.clear();
    }
}

// Hooks the deserializer bctrl (0x82D844C8); only stages the sidecar, the native load still runs.
void ReadGameplaySidecarBeforeDeserialize()
{
    // The practice classification reads the sidecar whatever save_sidecar says.
    if (!active || !active->load)
    {
        return;
    }
    active->pending.Stage(std::nullopt);
    active->weeklyClaim    = false;
    active->fallbackReason = "no native file captured";
    try
    {
        if (!active->nativeFile.empty())
        {
            active->fallbackReason = "schema or identity reject";
            sidecar::ReadFailure failure;
            auto                 state = sidecar::Read(active->nativeFile, readString(uint64_t{ active->record } + 0x108, 43), readWord(active->record), &failure, &active->weeklyClaim);
            switch (failure)
            {
                case sidecar::ReadFailure::None:
                    break;
                case sidecar::ReadFailure::Missing:
                    active->fallbackReason = "sidecar missing";
                    break;
                case sidecar::ReadFailure::Unreadable:
                    active->fallbackReason = "sidecar unreadable";
                    break;
                case sidecar::ReadFailure::DigestUnreadable:
                    active->fallbackReason = "native digest unreadable";
                    break;
                case sidecar::ReadFailure::DigestMismatch:
                    active->fallbackReason = "native digest mismatch";
                    break;
                case sidecar::ReadFailure::Rejected:
                    active->fallbackReason = "schema or identity reject";
                    break;
            }
            active->pending.Stage(std::move(state));
        }
    }
    catch (...)
    {
        active->fallbackReason = "sidecar read failed";
        active->pending.Stage(std::nullopt);
    }
}

void WriteGameplaySidecarAfterSave(PPCRegister& result, PPCRegister& filename)
{
    if (!active || active->load || result.u32 == 0)
    {
        return;
    }
    if (!active->enabled && !active->forced)
    {
        return;
    }
    if (active->forced)
    {
        REXLOG_INFO("Save sidecar: disabled, but a save of a practice or isolated game always writes its sidecar");
    }
    bool saved = false;
    try
    {
        if (!active->nativeFile.empty())
        {
            const auto state = captureState(active->manual);
            if (state.practiceMarker)
            {
                REXLOG_INFO("Save sidecar: this save is not a resumable practice save; it carries an isolation marker and loads isolated without practice rules");
            }
            saved = sidecar::Save(result.u32 != 0, active->nativeFile, readString(filename.u32, 43), readWord(active->record), state);
        }
    }
    catch (...)
    {
    }
    if (!saved)
    {
        REXLOG_WARN("Save sidecar: native save succeeded but gameplay sidecar could not be written");
        if (rerevved::gotw::ForGame().Isolates())
        {
            REXLOG_ERROR("Game of the Week: this save of a practice or isolated game has no sidecar and will load as an ordinary game whose results count; do not use it");
        }
    }
}

namespace
{

// A practice save restores its own Game of the Week policy before any rules reader.
void restorePractice(const rerevved::gotw::PracticeBlock& block) noexcept
{
    rerevved::gotw::RestorePractice(block);
    rerevved::gameplay_fixes::RestoreDominationMinimum(block.dominationMinimum);
    rerevved::SuspendModRulesForGame(block.standardRules);
}

} // namespace

namespace rerevved::save_sidecar
{

void RestoreAfterLoadReset() noexcept
{
    if (!active || !active->load)
    {
        return;
    }
    try
    {
        if (const auto& state = active->pending.AfterReset())
        {
            // Restore the Game of the Week origin before fallible steps, for load-guard-validated single-player blocks.
            if (state->practice)
            {
                if (active->practiceVerified && gotw::CurrentSetupIsSinglePlayer())
                {
                    restorePractice(*state->practice);
                }
                else
                {
                    REXLOG_INFO("Save sidecar: Game of the Week practice metadata not restored");
                }
            }
            if (!active->enabled)
            {
                return;
            }
            if (!resource_identity_overrides::Publish(state->overrides))
            {
                active->restoreFailed = true;
                return;
            }
            raging_barbarians::RestoreForGame(state->ragingBarbarians);
            occ::RestoreForGame({ state->occEnabled, state->occPopulation });
            start_resources::RestoreForGame(state->resources);
            ai_economy::RestoreForGame(state->aiEconomy);
            gameplay_fixes::RestoreTurnLimit(state->turnLimit);
            gameplay_fixes::RestoreAlwaysAtWar(state->alwaysAtWar);
            custom_achievements::RestoreProgress({ .occFromSetup       = state->occFromSetup,
                                                   .barbarianDefeats   = state->barbarianDefeats,
                                                   .historyKnown       = state->historyKnown,
                                                   .unitBuilt          = state->unitBuilt,
                                                   .battleFought       = state->battleFought,
                                                   .deepOceanEntered   = state->deepOceanEntered,
                                                   .firstFoundingTurn  = state->firstFoundingTurn,
                                                   .initialAiOpponents = state->initialAiOpponents,
                                                   .navalVictories     = state->navalVictories });
            if (!active->restoreLogged)
            {
                active->restoreLogged = true;
                REXLOG_INFO("Save sidecar: restored saved gameplay settings and overrides{}", state->practice ? " and Game of the Week practice policy" : "");
            }
        }
    }
    catch (...)
    {
        active->restoreFailed = true;
    }
}

} // namespace rerevved::save_sidecar

// Successful loads rejoin at 0x82D86598 (staging 0x82D844C8, reset 0x82D86674); r1 is the owner's header
// frame (saved week +176), r29 is incoming r8 (nonzero skips the seed writer). Never enter content-refusal.
void ClassifyGotwLoad(PPCRegister& sp, PPCRegister& writerFlag)
{
    using rerevved::gotw::LoadKind;
    rerevved::gotw::ArmLoadIsolation(false);
    try
    {
        const uint32_t                               nativeWeek = readWord(uint64_t{ sp.u32 } + 176);
        std::optional<rerevved::gotw::PracticeBlock> saved;
        if (active && active->load && active->pending.AfterReset())
        {
            saved = active->pending.AfterReset()->practice;
        }
        const bool claim     = active && active->load && active->weeklyClaim;
        const bool resumable = writerFlag.u32 == 0 && rerevved::gotw::CurrentSetupIsSinglePlayer();
        const auto kind      = rerevved::gotw::ClassifyLoad(nativeWeek, saved, claim, resumable);
        rerevved::gotw::ArmLoadIsolation(kind != LoadKind::Ordinary);
        if (active)
        {
            active->practiceVerified = kind == LoadKind::Practice;
        }
        if (kind == LoadKind::Isolated)
        {
            REXLOG_WARN("Game of the Week: load has a practice block ({}) that cannot resume here (native week {}, r8 {}); it loads without practice rules and its results stay isolated",
                        saved ? "present" : "rejected",
                        nativeWeek,
                        writerFlag.u32);
        }
    }
    catch (...)
    {
        rerevved::gotw::ArmLoadIsolation(true);
    }
}
