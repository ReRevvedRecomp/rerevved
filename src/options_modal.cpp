#include "options_modal.h"

namespace rerevved::options_modal
{

Route ChooseRoute(uint32_t returnAddress, uint32_t receiver, bool hostBound)
{
    if (hostBound && returnAddress == kPauseReturn && receiver == kPauseReceiver)
        return Route::kPage;
    return Route::kRetail;
}

uint64_t PageGate::Opened()
{
    return opened_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void PageGate::Closed(uint64_t id)
{
    uint64_t seen = closed_.load(std::memory_order_acquire);
    while (seen < id && !closed_.compare_exchange_weak(seen, id, std::memory_order_acq_rel))
    {
    }
}

void PageGate::Shutdown()
{
    shutdown_.store(true, std::memory_order_release);
}

bool PageGate::IsOpen(uint64_t id) const
{
    if (id == 0 || shutdown_.load(std::memory_order_acquire))
        return false;
    return id <= opened_.load(std::memory_order_acquire) && id > closed_.load(std::memory_order_acquire);
}

} // namespace rerevved::options_modal
