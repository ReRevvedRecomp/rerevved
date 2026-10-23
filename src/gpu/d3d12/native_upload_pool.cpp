#include "native_upload_pool.h"

#include <algorithm>
#include <bit>

namespace rerevved::gpu
{

std::uint64_t UploadPoolClassBytes(std::uint64_t bytes) noexcept
{
    if (bytes == 0 || bytes > (1ULL << 62))
        return 0;
    if (bytes <= kUploadPoolMinClassBytes)
        return kUploadPoolMinClassBytes;
    // base < bytes <= 2 * base; quarter-octave steps never go below 64 KiB.
    const auto base = std::bit_floor(bytes - 1);
    const auto step = std::max(base / 4, kUploadPoolMinClassBytes);
    return (bytes + step - 1) / step * step;
}

} // namespace rerevved::gpu
