#include "resource_settings.h"

namespace rerevved::resource_settings
{

Mode FromConfig(int32_t value)
{
    return value >= 0 && value <= 3 ? static_cast<Mode>(value) : Mode::Normal;
}

std::string_view Name(Mode mode)
{
    switch (mode)
    {
        case Mode::Balanced:
            return "Balanced";
        case Mode::Abundant:
            return "Abundant";
        case Mode::Legendary:
            return "Legendary";
        default:
            return "Normal";
    }
}

FrozenSelection::FrozenSelection(int32_t value)
: mode_(FromConfig(value))
{
}

std::optional<Mode> FrozenSelection::Consume()
{
    if (!pending_)
    {
        return std::nullopt;
    }
    pending_ = false;
    return mode_;
}

bool FrozenSelection::SuppressIndiaStartingResource() const
{
    return mode_ == Mode::Balanced || mode_ == Mode::Legendary;
}

} // namespace rerevved::resource_settings
