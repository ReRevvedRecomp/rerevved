#include "raging_barbarians_settings.h"

#include <atomic>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(raging_barbarians, false, "ReRevved", "Raging Barbarians for the next new game or save load");

namespace rerevved::raging_barbarians
{
namespace
{

std::atomic<bool> frozenEnabled{ false };

} // namespace

void FreezeForGame(bool standardRules)
{
    frozenEnabled.store(!standardRules && REXCVAR_GET(raging_barbarians), std::memory_order_relaxed);
}

Evaluation Evaluate(int32_t nativeValue) noexcept
{
    const bool enabled = frozenEnabled.load(std::memory_order_relaxed);
    return { nativeValue, enabled ? 2 : nativeValue, enabled };
}

void RestoreForGame(bool enabled) noexcept
{
    frozenEnabled.store(enabled, std::memory_order_relaxed);
}

bool IsCreationResultValid(int32_t owner, int32_t type, int32_t slot) noexcept
{
    return owner >= 0 && owner < 6 && type >= 3 && type <= 5 && slot >= 0 && slot < 256;
}

} // namespace rerevved::raging_barbarians
