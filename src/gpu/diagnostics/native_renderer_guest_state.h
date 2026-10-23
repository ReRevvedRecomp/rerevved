#pragma once

#include <cstdint>

namespace rerevved::gpu
{

bool PublishGuestDevice(std::uint32_t cellAddress,
                        std::uint32_t deviceAddress) noexcept;
void ResetGuestDevicePublication() noexcept;

std::uint64_t ObserveGuestTexture(std::uint32_t width,
                                  std::uint32_t height) noexcept;
void          ResetGuestTextureObservation() noexcept;

} // namespace rerevved::gpu
