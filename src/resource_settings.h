#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace rerevved::resource_settings
{

enum class Mode : int32_t
{
    Normal    = 0,
    Balanced  = 1,
    Abundant  = 2, // Reserved; not offered in the dropdown.
    Legendary = 3,
};

Mode             FromConfig(int32_t value);
std::string_view Name(Mode mode);

// A new setup owns one snapshot. Consuming it cannot observe later config edits.
class FrozenSelection
{
public:
    explicit FrozenSelection(int32_t value);
    std::optional<Mode> Consume();
    bool                SuppressIndiaStartingResource() const;

private:
    Mode mode_;
    bool pending_ = true;
};

} // namespace rerevved::resource_settings
