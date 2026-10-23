#pragma once

#include "gpu/guest_d3d/resource_mirror_registry.h"
#include "gpu/guest_d3d/solid_draw_adapter.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>

namespace rerevved::gpu::guest_d3d::testing
{

// Process totals of mirrored reads, printed at exit so a run shows that the
// mirrored path served its reads.
struct MirrorFixtureTotals
{
    std::atomic<std::uint64_t> bound{ 0 }, unbound{ 0 };

    ~MirrorFixtureTotals()
    {
        std::cout << "resource mirror fixture: bound=" << bound.load() << " unbound=" << unbound.load() << std::endl;
    }
};

inline MirrorFixtureTotals& MirrorFixtureCounts()
{
    static MirrorFixtureTotals totals;
    return totals;
}

// Mirror reads bind captured texture, stream and index ranges; the first
// differing byte aborts.
inline ReadMemory MirrorFixtureReads(ReadMemory read)
{
    char*       enabled = nullptr;
    std::size_t length  = 0;
    const bool  on      = _dupenv_s(&enabled, &length, "REREVVED_TEST_RESOURCE_MIRRORS") == 0 && enabled && std::string_view(enabled) == "1";
    std::free(enabled);
    if (!on)
        return read;

    struct Feeder
    {
        GuestResourceMirrors mirrors{ { .watch = [](std::uint32_t, std::uint32_t)
                                        {
                                            return true;
                                        } } };
        std::map<std::tuple<GuestResourceKind, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> headers;
        std::uint32_t                                                                                       nextHeader = 1; // never a guest object address
    };

    MirrorFixtureCounts();
    auto       feeder     = std::make_shared<Feeder>();
    const auto base       = static_cast<GuestMemoryRead>(read);
    read.mirroredResource = [feeder, base](const GuestResourceRead& request) -> std::optional<GuestBytes>
    {
        const auto fixture = base(request.address, request.size, true);
        if (fixture.size() != request.size)
            return std::nullopt;
        const auto range = [&](std::uint32_t address, std::uint32_t size)
        {
            return base(address, size, true);
        };
        auto bound = feeder->mirrors.Bind(request, range);
        if (!bound)
        {
            // An index buffer registers under the object the draw names.
            GuestResourceRegistration registration;
            registration.kind = request.kind;
            if (request.kind == GuestResourceKind::IndexBuffer && request.header)
                registration.header = request.header;
            else
            {
                auto& header = feeder->headers[{ request.kind, request.address, request.size, request.words[1] }];
                if (!header)
                    header = feeder->nextHeader++;
                registration.header = header;
            }
            registration.words     = request.words;
            registration.ranges[0] = { request.address, request.size };
            feeder->mirrors.Register(registration);
            bound = feeder->mirrors.Bind(request, range);
        }
        if (bound && !(*bound == fixture))
        {
            std::cerr << "resource mirror fixture: the bound read of kind " << int(request.kind) << " at " << std::hex << request.address << " size " << request.size << std::dec
                      << " differs from the fixture bytes\n";
            std::abort();
        }
        (bound ? MirrorFixtureCounts().bound : MirrorFixtureCounts().unbound).fetch_add(1, std::memory_order_relaxed);
        return bound;
    };
    return read;
}

} // namespace rerevved::gpu::guest_d3d::testing
