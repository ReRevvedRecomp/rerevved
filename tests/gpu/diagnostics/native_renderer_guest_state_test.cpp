#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("native_renderer_guest_state", "[ctest][native-renderer-guest-state]")
{
    using namespace rerevved::gpu;

    // Leaves the process-wide observation state as other cases expect it.
    struct ResetOnExit
    {
        ~ResetOnExit()
        {
            ResetGuestDevicePublication();
            ResetGuestTextureObservation();
        }
    } resetOnExit;

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();

    UNSCOPED_INFO("first publication changes state");
    REQUIRE(PublishGuestDevice(0xFFCAE000, 0x40123450));

    UNSCOPED_INFO("duplicate publication is stable");
    REQUIRE(!PublishGuestDevice(0xFFCAE000, 0x40123450));
    UNSCOPED_INFO("replacement publication changes state");
    REQUIRE(PublishGuestDevice(0xFFCAE000, 0x40167890));

    UNSCOPED_INFO("first texture sequence");
    REQUIRE(ObserveGuestTexture(1024, 256) == 1);
    UNSCOPED_INFO("second texture sequence");
    REQUIRE(ObserveGuestTexture(64, 64) == 2);

    ResetGuestTextureObservation();
    UNSCOPED_INFO("texture observation reset state");
    REQUIRE(ObserveGuestTexture(64, 64) == 1);
}
