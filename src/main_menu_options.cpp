#include "main_menu_options.h"

namespace rerevved::main_menu_options
{

Route ChooseRoute(const Entry& entry)
{
    if (!entry.hostBound || !entry.descriptorIsOptions || entry.parent == 0 || entry.menuVtable != kMenuVtable ||
        entry.parentVtable != kParentVtable || entry.parentHandler != kParentHandler || entry.pending != 0 || entry.exiting != 0 ||
        entry.currentState != entry.parent || entry.prompt != 0)
        return Route::kRetail;
    return entry.pageOpen ? Route::kIgnore : Route::kPage;
}

void Session::Begin(uint64_t page, uint32_t parent, uint32_t managerPending)
{
    std::lock_guard lock(mutex_);
    state_ = { page, parent, managerPending };
    active_.store(page, std::memory_order_release);
}

void Session::End(uint64_t page)
{
    std::lock_guard lock(mutex_);
    if (state_.page == page)
    {
        state_ = {};
        active_.store(0, std::memory_order_release);
    }
}

Session::State Session::Current() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

} // namespace rerevved::main_menu_options
