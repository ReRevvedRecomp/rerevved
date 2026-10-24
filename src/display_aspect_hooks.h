#pragma once

// With no pending correction and display_aspect::Active() false, these hooks
// leave guest bytes and control flow unchanged; raw wrappers call the original
// function exactly once.
// Raw wrappers (REX_HOOK_RAW):
//   sub_82C71A00  frame owner r3, camera r4: freezes the interval
//   sub_823528D8  renderer r3, camera r4: join; resets cached camera match
//   sub_823AF9C8  owner r3, camera [r3+0x20]: alternate join
//   sub_8250BA00  renderer r3, frustum r8, viewport r9: widens left/right
//                 words for the call only
//   sub_82C71880  frame owner r3, stage range r4..r5: stage census
//   sub_82C5EC28  movie owner r3: freezes or flags the interval
//   sub_823035D0  GFx display end
//   probe-only (observed, never mutated): 8234EB00, 8234CF18, 8234DF90,
//   8234D360, 82C5DF60, 8234D890, 82C49D08
// Midasm hooks: config/display_aspect_hooks.toml.
// Swap boundary: rerevved_hooks.cpp forwards ObserveRendererSwapSource here.
namespace rerevved::display_aspect
{

// Ends the swap interval: reads the gameplay state key, publishes the frozen
// policy to the SDK, writes probe records, starts the next. Safe when all off.
void OnGuestSwap();

} // namespace rerevved::display_aspect
