#include "presentation_fixture.h"

#include "admitted_presentation_frame.h"
#include "gpu/present/native_presentation.h"

#include <array>

namespace rerevved::gpu
{

bool LoadPresentationFixture(const std::filesystem::path& package, NativeRenderFrame& frame, std::string& error)
{
    std::array<std::uint32_t, 256> gamma;
    for (std::uint32_t i = 0; i < gamma.size(); ++i)
    {
        const auto value = (i * 1023U + 127U) / 255U;
        gamma[i]         = PackGammaEntry(value, value, value);
    }
    presentation_fixture::AdmittedFrame admitted;
    if (!presentation_fixture::BuildAdmittedFrame(package, gamma, admitted, error))
        return false;
    frame                      = std::move(admitted.frame);
    frame.returnResolvedOutput = false;
    return true;
}

} // namespace rerevved::gpu
