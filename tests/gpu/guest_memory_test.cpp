// guest_memory: the guarded host copy that tiling restore and the developer probes
// read guest memory through.

#include "gpu/guest_d3d/guest_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace rerevved::gpu::guest_d3d;

namespace
{

void testGuardedCopy()
{
    std::array<std::uint8_t, 8> source{ 1, 2, 3, 4, 5, 6, 7, 8 };
    std::array<std::uint8_t, 8> target{};
    UNSCOPED_INFO("a readable copy lands");
    REQUIRE((CopyGuestGuarded(target.data(), source.data(), source.size()) && target == source));
#if defined(_WIN32)
    // A page decommitted after the access query: the copy fails and the process survives.
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    UNSCOPED_INFO("the test page");
    REQUIRE(page != nullptr);
    target = {};
    UNSCOPED_INFO("a no-access page fails the copy");
    REQUIRE(!CopyGuestGuarded(target.data(), page, target.size()));
    UNSCOPED_INFO("an unmapped address fails the copy");
    REQUIRE(!CopyGuestGuarded(target.data(), reinterpret_cast<const void*>(16), target.size()));
    UNSCOPED_INFO("the test page is released");
    REQUIRE(VirtualFree(page, 0, MEM_RELEASE) != 0);
    UNSCOPED_INFO("a readable copy still lands after a fault");
    REQUIRE((CopyGuestGuarded(target.data(), source.data(), source.size()) && target == source));
#endif
}

#if defined(_WIN32)
void* raiseInPageError(void*, const void*, std::size_t)
{
    RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 0, nullptr);
    return nullptr;
}

void* raiseOther(void*, const void*, std::size_t)
{
    RaiseException(0xE0001234, 0, 0, nullptr);
    return nullptr;
}

bool otherPassesThrough()
{
    std::uint8_t target = 0;
    __try
    {
        CopyGuestGuardedWith(&raiseOther, &target, &target, 1);
        return false;
    }
    __except (GetExceptionCode() == 0xE0001234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return true;
    }
}
#endif

void testGuardedCopySeam()
{
#if defined(_WIN32)
    std::uint8_t target = 0;
    // A file-backed page that fails to load raises an in-page error, not an access violation.
    UNSCOPED_INFO("an in-page error fails the copy");
    REQUIRE(!CopyGuestGuardedWith(&raiseInPageError, &target, &target, 1));
    // Only those two faults are swallowed.
    UNSCOPED_INFO("another exception is not swallowed");
    REQUIRE(otherPassesThrough());
#endif
}

#if defined(_WIN32)
void* faultAccessViolation(void*, const void*, std::size_t)
{
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return nullptr;
}
#endif
void testGuardedFaultsAfterQuery()
{
#if defined(_WIN32)
    std::uint8_t byte = 0;
    UNSCOPED_INFO("access violation is unreadable");
    REQUIRE(!CopyGuestGuardedWith(faultAccessViolation, &byte, &byte, 1));
#endif
}

} // namespace

TEST_CASE("guest_memory", "[ctest][guest-memory][timeout-60]")
{
    testGuardedCopy();
    testGuardedCopySeam();
    testGuardedFaultsAfterQuery();
}
