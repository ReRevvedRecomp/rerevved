#include "save_sidecar.h"

#include "occ_settings.h"
#include "raging_barbarians_settings.h"
#include "start_resources_runtime.h"

#include <stdexcept>

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
    Transaction(uint32_t selectedRecord, bool loading)
    : previous(active)
    , record(selectedRecord)
    , load(loading)
    , enabled(REXCVAR_GET(save_sidecar))
    {
        active = this;
    }

    ~Transaction()
    {
        active = previous;
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

    Transaction*          previous;
    uint32_t              record;
    bool                  load;
    bool                  enabled;
    std::filesystem::path nativeFile;
    sidecar::PendingLoad  pending;
    const char*           fallbackReason = "sidecar read not reached";
    bool                  restoreLogged  = false;
    bool                  restoreFailed  = false;
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

sidecar::State captureState()
{
    const auto occ = rerevved::occ::ForGame();
    return { rerevved::raging_barbarians::Evaluate(0).enabled,
             occ->enabled,
             occ->minimumPopulation,
             rerevved::start_resources::ForGame(),
             *rerevved::resource_identity_overrides::Current(),
             rerevved::ai_economy::ForSave(rerevved::ai_economy::NativeCostsRequired()) };
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
    Transaction transaction(ctx.r4.u32, false);
    __imp__sub_82D82510(ctx, base);
}

REX_HOOK_RAW(sub_82D387E0)
{
    // The selected-content stream calls this opener at 0x82E83B38.
    // Its +0x10 string holds the normalized native guest filename on return.
    const bool     capture = active && active->enabled && ctx.lr == 0x82E83B3C;
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

void ReadGameplaySidecarBeforeDeserialize()
{
    if (!active || !active->enabled || !active->load)
    {
        return;
    }
    active->pending.Stage(std::nullopt);
    active->fallbackReason = "no native file captured";
    try
    {
        if (!active->nativeFile.empty())
        {
            active->fallbackReason = "schema or identity reject";
            sidecar::ReadFailure failure;
            auto                 state = sidecar::Read(active->nativeFile, readString(uint64_t{ active->record } + 0x108, 43), readWord(active->record), &failure);
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
    if (!active || !active->enabled || active->load || result.u32 == 0)
    {
        return;
    }
    bool saved = false;
    try
    {
        if (!active->nativeFile.empty())
        {
            saved = sidecar::Save(result.u32 != 0, active->nativeFile, readString(filename.u32, 43), readWord(active->record), captureState());
        }
    }
    catch (...)
    {
    }
    if (!saved)
    {
        REXLOG_WARN("Save sidecar: native save succeeded but gameplay sidecar could not be written");
    }
}

namespace rerevved::save_sidecar
{

void RestoreAfterLoadReset() noexcept
{
    if (!active || !active->enabled || !active->load)
    {
        return;
    }
    try
    {
        if (const auto& state = active->pending.AfterReset())
        {
            if (!resource_identity_overrides::Publish(state->overrides))
            {
                active->restoreFailed = true;
                return;
            }
            raging_barbarians::RestoreForGame(state->ragingBarbarians);
            occ::RestoreForGame({ state->occEnabled, state->occPopulation });
            start_resources::RestoreForGame(state->resources);
            ai_economy::RestoreForGame(state->aiEconomy);
            if (!active->restoreLogged)
            {
                active->restoreLogged = true;
                REXLOG_INFO("Save sidecar: restored saved gameplay settings and overrides");
            }
        }
    }
    catch (...)
    {
        active->restoreFailed = true;
    }
}

} // namespace rerevved::save_sidecar
