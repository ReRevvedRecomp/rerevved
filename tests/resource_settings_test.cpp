#include "resource_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace
{

} // namespace

TEST_CASE("resource_settings", "[ctest][resource-settings][local-fast]")
{
    using namespace rerevved::resource_settings;
    UNSCOPED_INFO("Normal is the default");
    CHECK((FromConfig(0) == Mode::Normal && Name(Mode::Normal) == "Normal"));
    UNSCOPED_INFO("Balanced retains config value one");
    CHECK((FromConfig(1) == Mode::Balanced && Name(Mode::Balanced) == "Balanced"));
    UNSCOPED_INFO("Abundant remains reserved");
    CHECK(FromConfig(2) == Mode::Abundant);
    UNSCOPED_INFO("Legendary retains config value three");
    CHECK((FromConfig(3) == Mode::Legendary && Name(Mode::Legendary) == "Legendary"));
    UNSCOPED_INFO("invalid settings fail closed");
    CHECK((FromConfig(-1) == Mode::Normal && FromConfig(4) == Mode::Normal));

    int             config = 1;
    FrozenSelection setup(config);
    config = 0;
    UNSCOPED_INFO("Balanced suppresses India's grant before setup consumes the selection");
    CHECK(setup.SuppressIndiaStartingResource());
    UNSCOPED_INFO("mid-setup config changes do not change the frozen selection");
    CHECK(setup.Consume() == Mode::Balanced);
    UNSCOPED_INFO("consumption retains the frozen policy until setup exits");
    CHECK(setup.SuppressIndiaStartingResource());
    UNSCOPED_INFO("a setup can be applied only once");
    CHECK(!setup.Consume());
    FrozenSelection nextSetup(config);
    UNSCOPED_INFO("a later new game samples the edited config");
    CHECK(nextSetup.Consume() == Mode::Normal);
    FrozenSelection legendary(3);
    UNSCOPED_INFO("Legendary suppresses India's grant");
    CHECK(legendary.SuppressIndiaStartingResource());
    for (int value : { -1, 0, 2, 4 })
    {
        FrozenSelection native(value);
        UNSCOPED_INFO("Normal and reserved values keep India's native path");
        CHECK(!native.SuppressIndiaStartingResource());
    }
}
