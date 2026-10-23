#pragma once

#include "rules_bundle.h"

#include <cstdint>

namespace rerevved::tests
{

// Makes rules-bundle preparation fail after `successfulSteps` steps and always
// disarms on scope exit. Needs the RULES_BUNDLE_TESTING variant of rules_bundle.
class ScopedPreparationFailure
{
public:
    explicit ScopedPreparationFailure(std::int64_t successfulSteps) noexcept
    {
        rules_bundle::FailPreparationAfterForTests(successfulSteps);
    }

    ~ScopedPreparationFailure()
    {
        rules_bundle::FailPreparationAfterForTests(-1);
    }

    ScopedPreparationFailure(const ScopedPreparationFailure&)            = delete;
    ScopedPreparationFailure& operator=(const ScopedPreparationFailure&) = delete;
};

} // namespace rerevved::tests
