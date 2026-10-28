#pragma once

#include <cstdint>

#include <rex/ppc.h>

// Combat observers for I Have No Enemies (RecordBattle) and Last Stand
// (RecordLastStand). Evidence: rerevved-research manifests/
// achievement-event-boundaries.json RVA-F-0359 and RVA-F-0360. The native queue
// hooks belong to capital grace and forward their lineage through
// capital_grace::QueueObserver, and the unit-kill hook in
// custom_achievement_hooks.cpp calls ObserveAttackerDefeat.
namespace rerevved::custom_achievement_combat
{

// One attacker-defeat call of sub_82D02390 as the kill hook observed it.
struct Defeat
{
    uint32_t lr         = 0; // 0x82CDD124 (resolver) or 0x82CD83DC (result handler)
    uint32_t frame      = 0; // r1 of the calling combat function
    int32_t  loser      = -1;
    uint32_t unit       = 0;
    int32_t  winner     = -1;
    int32_t  local      = -1;
    bool     liveBefore = false;
    bool     deadAfter  = false;
};

// Awards Last Stand for a qualifying defeat. Ignores every other kill.
void ObserveAttackerDefeat(const Defeat& defeat) noexcept;

// True for the nuclear victim-kill call sites, which bypass the mode 0 observers.
bool IsBypassKill(uint32_t lr) noexcept;
// Counts a strike as I Have No Enemies participation when the local player
// launched it (launcher, -1 unknown) or lost the unit. Traces the raw tuple.
void ObserveBypassKill(uint32_t lr, int32_t player, uint32_t unit, int32_t mode, int32_t local, int32_t launcher) noexcept;

// Resolver invocation scope, called from the sub_82CD9970 wrapper for mode 0.
void EnterResolver(uint32_t frame, int32_t attackerPlayer, uint32_t attackerUnit) noexcept;
void LeaveResolver(uint32_t frame, uint32_t result) noexcept;

// Drops every pending observation, such as on a game load or a new game.
void ResetCombatObservers() noexcept;

} // namespace rerevved::custom_achievement_combat

// Midasm hooks (config/custom_achievement_combat_hooks.toml).
void ArmCustomAchievementCombat(PPCRegister& stack);
void ObserveCustomAchievementCombatDecision(PPCRegister& stack);
void ObserveCustomAchievementCombatRng(PPCRegister& stack, PPCRegister& rngBlock, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr);
void BeginCustomAchievementCombatEmit(PPCRegister& stack, PPCRegister& type, PPCRegister& player, PPCRegister& composite, PPCRegister& third, PPCRegister& fourth);
void EndCustomAchievementCombatEmit(PPCRegister& stack);
void BeginCustomAchievementCombatResult(PPCRegister& stack, PPCRegister& player, PPCRegister& composite, PPCRegister& third);
void EndCustomAchievementCombatResult(PPCRegister& stack, PPCRegister& result);
