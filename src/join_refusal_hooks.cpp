#include "join_refusal_hooks.h"

#include "network_admission.h"

#include <atomic>

#include <rex/logging.h>

namespace
{

std::atomic<uint32_t> refusals{ 0 };

} // namespace

bool RefuseJoinInProgress(PPCRegister& result, PPCRegister& stateKey)
{
    namespace admission = rerevved::network_admission;
    if (!admission::IsGameInProgress(stateKey.u32))
    {
        return false;
    }
    result.u64 = admission::kRefusedResult;
    try
    {
        REXLOG_WARN("Network admission: refused a join while a game is in progress (refusal {})", ++refusals);
    }
    catch (...)
    {
    }
    return true;
}
