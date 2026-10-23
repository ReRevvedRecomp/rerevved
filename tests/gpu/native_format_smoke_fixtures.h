#pragma once

// CPU-only checks of one representative fixture per Prepared format family.
// Each throws std::runtime_error naming the first expectation that failed.
// The research executable keeps the exhaustive per-form guards.
namespace rerevved::tests::format_smoke
{

// Packed BC1 fixture: eight mips, k8in16 blocks.
void CheckBc1Fixture();
// rgba8-8-packed-base: uncompressed RGBA8 with a packed base prefix.
void CheckRgba8Fixture();
// r32-float-512-tiled: tiled R32_FLOAT.
void CheckFloatFixture();
// rgba8-cube-64-packed: six-face cube with packed tail mips.
void CheckCubeFixture();
// Eight-level linear RGBA8 register-LOD fixture.
void CheckLodFixture();

} // namespace rerevved::tests::format_smoke
