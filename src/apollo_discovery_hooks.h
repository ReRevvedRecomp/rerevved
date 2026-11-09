#pragma once

#include <cstdint>

// Clear pending Apollo completions on new game or load.
void ResetApolloDiscovery() noexcept;

uint32_t ApolloPendingCount() noexcept;
