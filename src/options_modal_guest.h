#pragma once

// Guest modal loop for an open Options page; see options_modal.h.

#include <cstdint>

#include <rex/ppc.h>

#include "native_settings.h"
#include "options_modal.h"

namespace rerevved::options_modal
{

// Mirrors runner 0x82E40738 (r4=r5=0) on the calling guest thread; copies ctx to a fresh frame below r1 and retains none.
Exit RunPageModal(PPCContext& ctx, uint8_t* base, const native_settings::HostBindings& host, uint64_t page);

} // namespace rerevved::options_modal
