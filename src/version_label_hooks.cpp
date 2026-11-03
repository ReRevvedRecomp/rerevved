#include "version_label.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(REREVVED_VERSION)
#define REREVVED_VERSION_LABEL_SOURCE REREVVED_VERSION
#else
// Tests do not define REREVVED_VERSION, which marks the title build.
#define REREVVED_VERSION_LABEL_SOURCE REREVVED_TEST_GAME_VERSION
#endif

namespace
{

std::mutex gMutex;
uint32_t   gLabel = 0;

} // namespace

// r6 is the SetVersion argument, normally the retail "v1.30"; the new string lives in the system heap for the process.
void ReplaceMainMenuVersion(PPCRegister& version)
{
    if (version.u32 != rerevved::version_label::kRetailVersionLiteral)
    {
        return;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
    {
        return;
    }
    std::lock_guard lock(gMutex);
    if (gLabel == 0)
    {
        // An allocation failure keeps the retail label and retries next time.
        const std::string text  = std::string("v") + REREVVED_VERSION_LABEL_SOURCE;
        const uint32_t    guest = memory->SystemHeapAlloc(static_cast<uint32_t>(text.size() + 1));
        if (guest == 0)
        {
            return;
        }
        std::memcpy(memory->TranslateVirtual<char*>(guest), text.c_str(), text.size() + 1);
        gLabel = guest;
    }
    version.u64 = gLabel;
}
