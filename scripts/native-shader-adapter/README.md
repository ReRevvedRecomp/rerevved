# Native shader adapter

This directory contains the inputs for reproducing the raw adapter used by
[native shader artifact generation](../../docs/native-shaders.md). The
reconstruction is pinned to upstream
https://github.com/zolaware/reblue-XenosRecomp.git at
339af41df2c23dbe3256c1c377716b81a0e0fe6b. The upstream license is retained in
[`LICENSE.md`](LICENSE.md).

## Reconstruct the adapter

`build_adapter.py` extracts pinned upstream blobs and initialized submodules into
a fresh generated tree, applies raw-interface.patch, then applies the separate
pixel-register-lod.patch, fetch-write-mask.patch, exec-clause-gate.patch,
register-lod-flow.patch and loop-constants.patch in that order. It never edits
the source repository. Put the generated build under `out/build/`. Windows
builds require Git, CMake, Ninja and a C++23 compiler initialized in the shell.
Use `--source-repo` with a checkout at the pinned revision and initialized
submodules; `--build-root` and `--diagnostics-root` must name absent directories.
`build_adapter.py --help` lists the arguments.

The focused stdlib unittest suite accepts NATIVE_SHADER_ADAPTER as the exact
executable path. Without that variable, it looks for
out/build/native-shader-adapter/cmake/raw_adapter.exe. If the external builds
are missing, executable checks are skipped. Checks that existing directories are
preserved still run.

## Translation contract

The executable accepts this raw interface:

```text
adapter --raw vs|ps words.le registers.le output.hlsl common.h hash
```

The title's raw_adapter.cpp decodes the register snapshot and passes it to
the selected pixel fetch checks. Descriptor LOD bias bits 12..21 at
0x4800 + 6 * fetchIndex + 4 must be zero, and selected relative fetch source or
destination addressing is rejected. The current input hash for the title's adapter
is recorded in manifest.json.

Pixel register LOD is admitted for an unpredicated normalized 2D fetch or the
canonical cube chain below with useRegLod set, register gradients clear, zero
LOD bias and zero coordinate offsets. The register LOD is state for each pixel that
only executed setters update, as in the SDK's
DxbcShaderTranslator::ProcessTextureFetchInstruction, which writes it under the
setter's predicate. An absolute scalar SetTextureLod is emitted in place,
wrapped in `if (p0)` or `if (!p0)` for a predicated instruction or a
cond_pred_exec clause. A fetch needs an earlier unconditional top-level setter,
so no pixel reads the unset value; a fetch reached only by conditional setters
is refused. useCompLod with register LOD follows the same translator: the
register LOD scales the coarse screen gradients by exp2(lod) through
tfetch2DComputedRegisterLod. It is admitted only for a top-level 2D fetch whose
mip filter is not base-map. Unsupported modes fail before creating the output
file. Ordinary implicit sampling and the existing vertex level zero
specialization remain on their existing paths.

Register LOD needs structured control flow. In a pixel shader with a
SetTextureLod or register LOD fetch, register-lod-flow.patch proves a
predicated if/else diamond: a predicated jump over a then block that ends in
the opposite predicated jump past the else block, with no p0 writer in the
then block and no other branch into it. The second jump then structures as
unconditional. Any other predicated jump keeps the upstream switch(pc)
fallback, which register LOD refuses. Shaders without register LOD skip the
proof and use the upstream translation.

exec-clause-gate.patch gives cond_pred_exec clauses in both stages the SDK's
clause semantics. As DxbcShaderTranslator::UpdateExecConditionalsAndEmitDisassembly,
the whole clause body runs under a test of p0 on clause entry, so a setp inside
it does not skip the rest. Instruction predicates inside still test the current
p0. A clause that ends the shader returns only from inside its gate, as
ProcessExecInstructionEnd breaks out within the open exec conditional. Upstream
already gates cexec bool clauses the same way. When a conditional or nested
end is the last clause, the alpha test or half-pixel offset epilogue is also
emitted after it, because the SDK completes the shader after its main loop.

loop-constants.patch reads each loop's SHADER_CONSTANT_LOOP word at draw
time, as the SDK's DXBC translator does: loop shaders declare
g_LoopConstantsArr at c21 of the shared constants (bytes 336..463 hold
i0..i31, after the booleans), and loop-free shaders are unchanged. Count is
bits 0..7 (zero skips the loop), start bits 8..15 and signed step bits
16..23 (xenos.h LoopConstant); reserved bits are ignored. aL is saved and
restored around each loop, and repeat loops keep the outer aL, matching
ProcessLoopStartInstruction and ProcessLoopEndInstruction. Only canonical
loops are admitted: loop_start skips to the instruction after its loop_end,
and loop_end jumps back to the first body instruction. Other targets are
refused as noncanonical, as are predicated breaks and calls.
A vertex fetch whose destination swizzle writes no component (only keep,
zero or one) emits no fetch assignment, only its constant components; dxc
rejects the empty write mask.

The cube register LOD specialization admits one cube ALU, its absolute
reciprocal and multiply-add, and one cube fetch using result.yxw. The frozen
ALU chain uses direction r0.xyz, result r1, and absolute c0.x = 1.5. Its
words are authored and decoded with SDK ucode structures in the cube fixture.
All existing register LOD guards apply. Changing face coordinates, the chain,
the cube count or the index input fails before output publication. The emitted
cube operation restores r0.xyz, records one direction, and passes the W index
and registerLod to TextureCube.SampleLevel in resource space 2. This supports
the authored fixture, without accepting general cube face manipulation.
