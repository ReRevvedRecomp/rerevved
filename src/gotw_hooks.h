#pragma once

#include "gameplay_session_settings.h"

#include <cstdint>

#include <rex/ppc.h>

namespace rerevved::gotw::detail
{

using WordReader = bool (*)(uint64_t address, uint32_t& value) noexcept;
// The hook supplies the checked guest reader; tests can supply setup snapshots.
void ApplySeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed, WordReader read, gameplay_session::detail::SnapshotProvider capture = nullptr) noexcept;
void ReapplySeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed, WordReader read) noexcept;
// Replaces the writer's derived civilization only for the armed accepted launch.
void OverrideCivilization(PPCRegister& civilization, PPCRegister& setup, PPCRegister& count) noexcept;

} // namespace rerevved::gotw::detail

void ApplyGotwSeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept;
void ReapplyGotwSeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept;
void OverrideGotwCivilization(PPCRegister& civilization, PPCRegister& setup, PPCRegister& count) noexcept;
void LogGotwSeed(PPCRegister& setupGlobalBase, PPCRegister& week) noexcept;
void TraceGotwWriterEntry(uint64_t lr, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) noexcept;
void TraceGotwWriterExit(uint64_t lr, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& setup) noexcept;
void TraceGotwGeneratorSeed(PPCRegister& setup) noexcept;
void TraceGotwPrng(PPCRegister& seed) noexcept;
void TraceGotwProcedural() noexcept;
void TraceGotwNamedLoad(PPCRegister& result) noexcept;
bool GuardGotwOutcome(PPCRegister& mode) noexcept;
bool GuardGotwScoreCache() noexcept;
bool GuardGotwWeekCache() noexcept;
bool GuardGotwSubmission() noexcept;
