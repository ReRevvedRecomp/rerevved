#include "guest_import_doubles.hpp"

#include "helper_commands.hpp"

#include <cstdio>
#include <map>
#include <mutex>

#include <rex/hook.h>

namespace rerevved::tests::guest_imports
{
namespace
{

// Built on first use, never during static initialization.
struct Registry
{
    std::mutex                                               mutex;
    std::map<std::uint32_t, std::shared_ptr<ImportCallback>> callbacks;
    std::map<std::uint32_t, std::uint64_t>                   calls;
    std::shared_ptr<ImportCallback>                          fallback;
};

Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

void Dispatch(std::uint32_t address, PPCContext& ctx, std::uint8_t* base)
{
    std::shared_ptr<ImportCallback> callback;
    {
        auto&                       registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        ++registry.calls[address];
        const auto found = registry.callbacks.find(address);
        callback         = found != registry.callbacks.end() ? found->second : registry.fallback;
    }
    if (callback && *callback)
    {
        (*callback)(address, ctx, base);
    }
}

} // namespace

ScopedImport::ScopedImport(std::uint32_t address, ImportCallback callback)
: address_(address)
{
    // Allocate before touching the registry: a throw here must leave the
    // current callback installed.
    auto                        replacement = std::make_shared<ImportCallback>(std::move(callback));
    auto&                       registry    = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto&                       slot = registry.callbacks[address];
    previous_                        = std::move(slot);
    slot                             = std::move(replacement);
}

ScopedImport::~ScopedImport()
{
    auto&                       registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (previous_)
    {
        registry.callbacks[address_] = std::move(previous_);
    }
    else
    {
        registry.callbacks.erase(address_);
    }
}

ScopedFallbackImport::ScopedFallbackImport(ImportCallback callback)
{
    auto                        replacement = std::make_shared<ImportCallback>(std::move(callback));
    auto&                       registry    = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    previous_         = std::move(registry.fallback);
    registry.fallback = std::move(replacement);
}

ScopedFallbackImport::~ScopedFallbackImport()
{
    auto&                       registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.fallback = std::move(previous_);
}

std::uint64_t ImportCallCount(std::uint32_t address)
{
    auto&                       registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto                  found = registry.calls.find(address);
    return found == registry.calls.end() ? 0 : found->second;
}

} // namespace rerevved::tests::guest_imports

// One extern "C" definition per covered symbol. Each forwards to Dispatch.
#define REREVVED_GUEST_IMPORT_DOUBLE(address)                               \
    extern "C" REX_FUNC(__imp__sub_##address)                               \
    {                                                                       \
        ::rerevved::tests::guest_imports::Dispatch(0x##address, ctx, base); \
    }

#define ORIGINAL_New(address) REREVVED_GUEST_IMPORT_DOUBLE(address)
#define ORIGINAL_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ORIGINAL_##mode(address)
#include "gpu/guest_d3d/route_entries.inc"
#undef ROUTE_ENTRY
#undef ORIGINAL_Existing
#undef ORIGINAL_New

// The census wrappers are production code; only their generated bodies are doubles.
#define CENSUS_ORIGINAL_Census(address) REREVVED_GUEST_IMPORT_DOUBLE(address)
#define CENSUS_ORIGINAL_Route(address)
#define CENSUS_ORIGINAL_Raw(address)
#define GUEST_API_ENTRY(address, role, name, hook) CENSUS_ORIGINAL_##hook(address)
#include "gpu/guest_d3d/guest_api_entries.inc"
#undef GUEST_API_ENTRY
#undef CENSUS_ORIGINAL_Census
#undef CENSUS_ORIGINAL_Route
#undef CENSUS_ORIGINAL_Raw

REREVVED_GUEST_IMPORT_DOUBLE(825107A0)
REREVVED_GUEST_IMPORT_DOUBLE(8269B118)
REREVVED_GUEST_IMPORT_DOUBLE(8269BD98)
REREVVED_GUEST_IMPORT_DOUBLE(8269C400)
REREVVED_GUEST_IMPORT_DOUBLE(826A9BA0)
REREVVED_GUEST_IMPORT_DOUBLE(826AB740)
REREVVED_GUEST_IMPORT_DOUBLE(826AC0A8)
REREVVED_GUEST_IMPORT_DOUBLE(826AC218)
REREVVED_GUEST_IMPORT_DOUBLE(826A2FF0)
REREVVED_GUEST_IMPORT_DOUBLE(826A3000)
REREVVED_GUEST_IMPORT_DOUBLE(826A3568)
REREVVED_GUEST_IMPORT_DOUBLE(826A39F8)
REREVVED_GUEST_IMPORT_DOUBLE(826A7040)
REREVVED_GUEST_IMPORT_DOUBLE(826A7138)
REREVVED_GUEST_IMPORT_DOUBLE(826AD150)
REREVVED_GUEST_IMPORT_DOUBLE(82CF0CB0)

// Guest helper functions the generated code would define, which the ring stub
// route test replaces with scripted bodies. They share the address space of the
// imports above.
#define REREVVED_GUEST_FUNCTION_DOUBLE(address)                             \
    extern "C" REX_FUNC(sub_##address)                                      \
    {                                                                       \
        ::rerevved::tests::guest_imports::Dispatch(0x##address, ctx, base); \
    }

REREVVED_GUEST_FUNCTION_DOUBLE(8269B508)
REREVVED_GUEST_FUNCTION_DOUBLE(8269B550)
REREVVED_GUEST_FUNCTION_DOUBLE(8269B818)
REREVVED_GUEST_FUNCTION_DOUBLE(8269C170)
REREVVED_GUEST_FUNCTION_DOUBLE(8269C270)

namespace
{

// Crosses the registry through a real double and checks scope restoration:
// `rerevved_tests --helper support.guest-imports-selfcheck`.
REREVVED_HELPER_COMMAND(guestImportsSelfCheck, "support.guest-imports-selfcheck")
{
    using namespace rerevved::tests::guest_imports;
    (void)args;
    int  failures = 0;
    auto expect   = [&](bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "guest import self-check failed: %s\n", what);
            ++failures;
        }
    };
    PPCContext   ctx{};
    std::uint8_t memory = 0;
    __imp__sub_826A3568(ctx, &memory);
    expect(ctx.r3.u64 == 0 && memory == 0, "default double changes nothing");
    {
        ScopedImport scope(0x826A3568, [](std::uint32_t address, PPCContext& c, std::uint8_t* base)
                           {
                               c.r3.u64 = address;
                               ++*base;
                           });
        __imp__sub_826A3568(ctx, &memory);
        expect(ctx.r3.u64 == 0x826A3568 && memory == 1, "scoped callback receives the address and context");
        {
            ScopedFallbackImport fallback([](std::uint32_t, PPCContext& c, std::uint8_t*)
                                          {
                                              c.r4.u64 = 7;
                                          });
            __imp__sub_826A3000(ctx, &memory);
            __imp__sub_826A3568(ctx, &memory);
            expect(ctx.r4.u64 == 7 && memory == 2, "fallback covers unscoped addresses only");
        }
        ctx.r4.u64 = 0;
        __imp__sub_826A3000(ctx, &memory);
        expect(ctx.r4.u64 == 0, "fallback scope restores the default");
    }
    ctx.r3.u64 = 0;
    __imp__sub_826A3568(ctx, &memory);
    expect(ctx.r3.u64 == 0 && memory == 2, "address scope restores the default");
    expect(ImportCallCount(0x826A3568) == 4, "calls are counted per address");
    return failures == 0 ? 0 : 1;
}

} // namespace
