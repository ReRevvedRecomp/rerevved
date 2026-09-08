#pragma once

#include <cstdint>

#include <game_ids.h>

namespace rerevved::unit_catalog
{

// Copies complete 32-bit fields from a producer record into caller storage.
// The producer's first field contains its current byte size.
int32_t CopySizedOutput(void*       out,
                        uint32_t    out_size,
                        const void* producer,
                        uint32_t    producer_size,
                        uint32_t    minimum_prefix);

bool TryResolveUnitIdentity(ReRevvedCivilizationId  civilization,
                            ReRevvedUnitTypeId      unit_type,
                            ReRevvedUnitIdentityId& identity);

} // namespace rerevved::unit_catalog
