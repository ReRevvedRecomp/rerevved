// One representative Prepared fixture per format family. The remaining forms,
// the Original-versus-native GPU proofs and their helper commands live in
// rerevved_prepared_tests (tests/gpu/prepared_tests.cmake).
#include "native_format_smoke_fixtures.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("native_prepared_bc1_fixture", "[ctest][native-prepared-bc1]")
{
    UNSCOPED_INFO("the packed BC1 fixture holds");
    REQUIRE_NOTHROW(rerevved::tests::format_smoke::CheckBc1Fixture());
}

TEST_CASE("native_prepared_rgba8_matrix_fixture", "[ctest][native-prepared-rgba8-matrix]")
{
    UNSCOPED_INFO("the rgba8-8-packed-base fixture holds");
    REQUIRE_NOTHROW(rerevved::tests::format_smoke::CheckRgba8Fixture());
}

TEST_CASE("native_prepared_float_matrix_fixture", "[ctest][native-prepared-float-matrix]")
{
    UNSCOPED_INFO("the r32-float-512-tiled fixture holds");
    REQUIRE_NOTHROW(rerevved::tests::format_smoke::CheckFloatFixture());
}

TEST_CASE("native_prepared_cube_matrix_fixture", "[ctest][native-prepared-cube-matrix]")
{
    UNSCOPED_INFO("the rgba8-cube-64-packed fixture holds");
    REQUIRE_NOTHROW(rerevved::tests::format_smoke::CheckCubeFixture());
}

TEST_CASE("native_pixel_lod_fixture", "[ctest][native-pixel-lod]")
{
    UNSCOPED_INFO("the eight-level LOD fixture holds");
    REQUIRE_NOTHROW(rerevved::tests::format_smoke::CheckLodFixture());
}
