#pragma once

// Puts a console variable back to the value it had when a test case began, so
// one case's settings cannot reach the next case in the same process.
//
//   REREVVED_RESTORE_CVAR(one_city_challenge);  // after REXCVAR_DECLARE

#include <rex/cvar.h>

#include <type_traits>
#include <utility>

namespace rerevved::tests
{

template <typename Value, typename Restore>
class ScopedCvarRestore
{
public:
    ScopedCvarRestore(Value saved, Restore restore)
    : saved_(std::move(saved))
    , restore_(std::move(restore))
    {
    }

    ~ScopedCvarRestore()
    {
        restore_(saved_);
    }

    ScopedCvarRestore(const ScopedCvarRestore&)            = delete;
    ScopedCvarRestore& operator=(const ScopedCvarRestore&) = delete;

private:
    Value   saved_;
    Restore restore_;
};

template <typename Value, typename Restore>
ScopedCvarRestore(Value, Restore) -> ScopedCvarRestore<std::decay_t<Value>, Restore>;

} // namespace rerevved::tests

#define REREVVED_RESTORE_CVAR(name)                               \
    const ::rerevved::tests::ScopedCvarRestore name##CvarRestore( \
        REXCVAR_GET(name),                                        \
        [](const auto& value)                                     \
        {                                                         \
            REXCVAR_SET(name, value);                             \
        })
