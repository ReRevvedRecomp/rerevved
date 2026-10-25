#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <rex/ppc/context.h>

// One definition per imported guest-function symbol that several GPU test
// sources used to define for themselves. The generated `__imp__sub_<address>`
// "Original" symbols are C-linkage, so anonymous namespaces cannot keep the
// per-test copies apart once every test links into one executable.
//
// Every double dispatches to a callback installed for the current scope. With
// no callback installed a double returns without touching the context or guest
// memory. Scopes nest and must end in reverse order of construction; the
// callback is copied before it runs, so it may install or end scopes. Callbacks
// may run on any thread that reaches the symbol, so a callback shared across
// threads must guard its own state.
//
// Covered symbols (see the interfaces notes for the inventory):
//   - every `New` row of gpu/guest_d3d/route_entries.inc,
//   - every `Census` row of gpu/guest_d3d/guest_api_entries.inc,
//   - __imp__sub_825107A0,
//   - __imp__sub_826A2FF0, 826A3000, 826A3568, 826A39F8, 826A7040, 826A7138
//     and 826AD150 (the raw draw entries),
//   - __imp__sub_8269B118, 8269BD98, 8269C400, 826A9BA0, 826AB740, 826AC0A8
//     and 826AC218 (the ring stub and tiling Originals),
//   - the guest helper functions sub_8269B508, 8269B550, 8269B818, 8269C170
//     and 8269C270, which the ring stub route test scripts.
//   - __imp__sub_82CF0CB0 (the civilization bonus lookup).
namespace rerevved::tests::guest_imports
{

// Receives the guest address of the called import, as in `0x826A3568`.
using ImportCallback = std::function<void(std::uint32_t address, PPCContext& ctx, std::uint8_t* base)>;

// Installs `callback` for one address; restores the previous callback on exit.
class ScopedImport
{
public:
    ScopedImport(std::uint32_t address, ImportCallback callback);
    ~ScopedImport();
    ScopedImport(const ScopedImport&)            = delete;
    ScopedImport& operator=(const ScopedImport&) = delete;

private:
    std::uint32_t                   address_;
    std::shared_ptr<ImportCallback> previous_;
};

// Installs `callback` for every covered address that has no ScopedImport of
// its own. Restores the previous fallback on exit.
class ScopedFallbackImport
{
public:
    explicit ScopedFallbackImport(ImportCallback callback);
    ~ScopedFallbackImport();
    ScopedFallbackImport(const ScopedFallbackImport&)            = delete;
    ScopedFallbackImport& operator=(const ScopedFallbackImport&) = delete;

private:
    std::shared_ptr<ImportCallback> previous_;
};

// Calls reaching `address` since process start, installed callback or not.
std::uint64_t ImportCallCount(std::uint32_t address);

} // namespace rerevved::tests::guest_imports
