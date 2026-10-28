#pragma once

#include "capital_grace.h"

namespace rerevved::capital_grace
{

enum class SpecialRoute
{
    Nuclear,
    Type24
};
enum class SpecialDecision
{
    Native,
    RefuseProtected,
    RefuseUnsupported
};

enum class SpecialReason
{
    Type24Capability,
    NuclearCapability,
    NuclearCapacity,
    NuclearFrame,
    NuclearLateBinding,
    ProtectedFootprint,
    Count
};

struct SpecialInput
{
    SpecialRoute route;
    Words        words;
    Actor        actor;
    int32_t      year;
    int32_t      barbarian;
};

// Reads are checked at the guest boundary. Tests supply literal guest bytes.
struct SpecialMemory
{
    void* context;
    bool (*read)(void*, uint32_t, void*, uint32_t) noexcept;
    uint32_t Word(uint32_t address) const;
    int32_t  Signed(uint32_t address) const;
    uint16_t Half(uint32_t address) const;
    int32_t  Byte(uint32_t address) const;
    void     Range(uint32_t address, uint32_t size) const;
};

// The caller has checked frozen policy, year and current Barbarian actor.
SpecialDecision AdmitSpecial(const SpecialInput& input) noexcept;
SpecialDecision AdmitSpecial(const SpecialMemory& memory, const SpecialInput& input) noexcept;
void            BeginSpecialDispatch(uint32_t frame) noexcept;
uint64_t        SpecialRefusalCount(SpecialReason reason) noexcept;

} // namespace rerevved::capital_grace
