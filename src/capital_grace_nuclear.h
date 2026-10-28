#pragma once

#include "capital_grace_special.h"

namespace rerevved::capital_grace
{

struct NuclearVisit
{
    Point   point;
    int32_t index;
};

std::optional<NuclearVisit> DecodeNuclearVisit(int32_t packed, int32_t width, int32_t height, int32_t dx, int32_t dy) noexcept;
bool                        NuclearAdmission(const SpecialMemory& memory, const SpecialInput& input);
bool                        NuclearPopulation(const SpecialMemory& memory, uint32_t offset, uint32_t populationBase, Point visit, int32_t barbarian);
bool                        NuclearVictim(const SpecialMemory& memory, int32_t bank, int32_t slot, uint32_t flagsAddress, Point visit, int32_t barbarian);

// Exact helper/dispatcher frames bind the consumption continuation. A new
// dispatcher admission expires any previous use of that same stack frame.
class NuclearInvocations
{
public:
    struct Invocation
    {
        uint32_t frame         = 0;
        uint32_t parent        = 0;
        uint32_t returnAddress = 0;
        int32_t  player        = -1;
        int32_t  slot          = -1;
        bool     causal        = false;
        bool     alternate     = false;
        bool     refused       = false;
    };

    void        BeginDispatch(uint32_t frame) noexcept;
    bool        Begin(Invocation invocation) noexcept;
    Invocation* Find(uint32_t frame, bool alternate) noexcept;
    void        End(uint32_t frame, bool consumption) noexcept;
    bool        Consume(uint32_t frame, int32_t player, int32_t slot) noexcept;
    size_t      Occupied() const noexcept;

private:
    std::array<Invocation, 16> frames_{};
    std::array<Invocation, 16> tokens_{};
};

} // namespace rerevved::capital_grace
