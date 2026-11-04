#include "network_admission.h"

#include "gameplay_session_settings.h"

namespace rerevved::network_admission
{

std::optional<Transition> ReadTransition(Memory& memory, uint32_t app, uint32_t updateResult)
{
    if (app == 0 || app > UINT32_MAX - kRequestedField)
    {
        return std::nullopt;
    }
    Transition transition;
    transition.app          = app;
    transition.updateResult = updateResult;
    const auto root         = memory.Word(kAppGlobal);
    const auto current      = memory.Word(app + kCurrentField);
    const auto pending      = memory.Word(app + kPendingField);
    const auto requested    = memory.Word(app + kRequestedField);
    const auto network      = memory.Byte(kNetworkByte);
    const auto setup        = memory.Word(kSetupGlobal);
    if (!root || !current || !pending || !requested || !network || !setup)
    {
        return std::nullopt;
    }
    transition.root         = *root;
    transition.current      = *current;
    transition.pending      = *pending;
    transition.requestedKey = *requested;
    transition.network      = *network;
    transition.setup        = *setup;
    if (*current != 0)
    {
        const auto key = *current > UINT32_MAX - kStateKeyField ? std::nullopt : memory.Word(*current + kStateKeyField);
        if (!key)
        {
            return std::nullopt;
        }
        transition.currentKey = *key;
    }
    if (*setup != 0)
    {
        const auto mode = *setup > UINT32_MAX - kModeField ? std::nullopt : memory.Word(*setup + kModeField);
        if (!mode)
        {
            return std::nullopt;
        }
        transition.mode = static_cast<int32_t>(*mode);
    }
    return transition;
}

bool IsNetworkGameStart(const Transition& transition) noexcept
{
    return (transition.updateResult & 0xFF) != 0 && transition.app != 0 && transition.app == transition.root &&
           transition.current != 0 && transition.currentKey == kFrontendKey && transition.pending == 0 &&
           transition.requestedKey == kGameKey && transition.network != 0 && transition.setup != 0 &&
           gameplay_session::ClassifySetupMode(transition.mode) == gameplay_session::SetupKind::Network;
}

Recheck DecideRecheck(const Transition& current, const std::optional<Transition>& after, bool retried) noexcept
{
    if (!after)
    {
        return retried ? Recheck::Refuse : Recheck::NoAction;
    }
    if (*after == current)
    {
        return Recheck::Apply;
    }
    if (!IsNetworkGameStart(*after))
    {
        return Recheck::NoAction;
    }
    return retried ? Recheck::Refuse : Recheck::Retry;
}

bool IsGameInProgress(uint32_t currentStateKey) noexcept
{
    return currentStateKey == kGameKey;
}

std::optional<ReturnRoute> MatchFrontendReturn(Memory& memory)
{
    const auto manager  = memory.Word(kManagerGlobal);
    const auto frontend = memory.Word(kFrontendManagerGlobal);
    if (!manager || *manager == 0 || !frontend || *frontend == 0 || *frontend > UINT32_MAX - kControllerField)
    {
        return std::nullopt;
    }
    const auto controller = memory.Word(*frontend + kControllerField);
    if (!controller || *controller == 0)
    {
        return std::nullopt;
    }
    const auto vtable = memory.Word(*controller);
    if (!vtable || *vtable == 0 || *vtable > UINT32_MAX - 4 * kMaxVtableSlots)
    {
        return std::nullopt;
    }
    for (uint32_t slot = 0; slot < kMaxVtableSlots; ++slot)
    {
        const auto entry = memory.Word(*vtable + 4 * slot);
        if (!entry)
        {
            return std::nullopt;
        }
        if (*entry == kFrontendReturn)
        {
            return ReturnRoute{ *controller, slot };
        }
    }
    return std::nullopt;
}

} // namespace rerevved::network_admission
