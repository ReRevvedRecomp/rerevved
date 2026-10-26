# Test authoring

A test must catch a concrete defect or protect an uncovered contract.
Count fixture setup, runtime, and source volume before adding a case.

## When to add a test

- A change fixes a demonstrated defect. Use the narrowest case that fails
  without the fix.
- A change adds a branch, error order, byte layout, or saved state that existing
  assertions do not cover. Keep public C header checks compiled as C.
- A guest hook must preserve registers, Original calls, or instruction order.
  Assert the observed contract at the hook boundary.
- Publication, allocation, restore, or shutdown can fail partway. Check that
  earlier state survives and owned resources are released.

## When not to add a test

- Another ID or format takes the same branch with the same assertions.
- Expected output comes from the function or registry being checked.
- A guard returns before the behavior under test runs.
- A native case only checks file names or source spelling. Put necessary
  repository contracts in Python checks and test behavior through a native seam.
- A GPU or application run only repeats a CPU fixture check.

## Choose the cheapest fixture that proves the claim

Pick the first level that can expose the defect:

1. Literal inputs to a function or decoder, as in
   [rush cost](../../tests/rush_cost_test.cpp).
2. Authored data or synthetic CPU fixtures, as in the
   [format smoke cases](../../tests/gpu/native_format_smoke_test.cpp).
3. A registry, Lua publication, or guest hook seam without the application.
   [Mod script rules](../../tests/mod_script_rules_test.cpp) exercise publication.
4. A GPU fixture with prepared inputs, as in the
   [BC1 proof](../../tests/gpu/native_prepared_bc1_test.cpp).
5. A bounded presentation lifecycle through
   [native_app_harness.py](../../tests/gpu/native_app_harness.py).
   Use its full application mode only when the presentation session is insufficient.

A fixture or hidden window does not establish retail gameplay correctness.
[Evidence and claims](evidence-and-claims.md) defines that boundary.

## Products and matrices

Check every authored row at the data level. Choose integrated representatives
by distinct format, packing, mip, dimension, update, and failure mechanism.
Explain what each representative covers. Preserve literal expected values.

The default format smoke cases keep one representative per family.
[Prepared tests](../../tests/gpu/prepared_tests.cmake) own the full matrices
in `rerevved_prepared_tests`, enabled on Windows with `REREVVED_PREPARED_TESTS=ON`.
Their `prepared-research` CTest cases check CPU fixtures. GPU comparisons use
the Python runners with `--test-exe` and prepared catalogs.
Repeat faults only for distinct failure paths or partial states. When reducing
a matrix, state the lost coverage and retain its data checks in the same change.

## Scaffolding

Reuse `tests/support` helpers instead of copying fakes or process launchers.
Use [ScopedRegistryReset](../../tests/support/registry_scope.hpp) for registries,
[REREVVED_RESTORE_CVAR](../../tests/support/cvar_scope.hpp) for changed settings,
[ScopedImport](../../tests/support/guest_import_doubles.hpp) for guest imports,
and [ScopedDirectoryRemoval](../../tests/support/directory_removal.hpp) for a
temporary directory a case creates.
An unconfigured import double does nothing. Assert calls that matter.

[Allocation scopes](../../tests/support/allocation_harness.hpp) restore failure
injection. Leave the scope before Catch2 assertions or messages allocate.
Use [CallbackChecks](../../tests/support/callback_checks.hpp) in worker callbacks
and call `Verify()` on the test thread.

Keep separate behaviors in named `TEST_CASE`s. A fatal assertion in one large
case prevents later checks from running. Give every case one suite tag.
[Helper commands](../../tests/support/helper_commands.hpp) own fatal children
and fixture emitters. Do not add another test executable for each family.

## Registration and checks

Add native sources to [the test target](../../tests/rerevved_tests.cmake) or its
GPU includes. Use `[ctest]` for automatic cases. Keep `[local-fast]` membership
consistent with [its allowlist](../../scripts/tests/test_local_fast_contract.py).
Use `[native_gpu]`, `[run_serial]`, and `[timeout-N]` only for their scheduling
needs. CTest applies these tags; direct executable runs do not take its lock.
Keep manual cases hidden with `[.][manual]`. Renderer research uses
`[.][renderer-research]` and optional `REREVVED_REGISTER_RESEARCH_TESTS` discovery.

[Development](../development.md#checks-and-tests) owns commands and prerequisites.
Select a native case with `ctest -R`, or use `ctest --preset win-amd64-local-fast`.
Keep CTest at `-j 2`. Recorder cases require `REREVVED_DEVELOPER`.
`scripts/verify.ps1` discovers Python tests in `scripts/tests`; developer CTest
also runs the analysis tests in `tests/gpu`. Use `unittest` for Python additions.

Use [generated_scan.py](../../scripts/tests/generated_scan.py) for generated
hook windows. Report missing generated inputs as skips, not successful coverage.
Keep generated guest code and retail inputs out of fixtures.
Distinguish missing optional catalogs from invalid supplied inputs. Record skips.
If Windows discovery fails before cases run, check runtime DLL staging in
[render_tests.cmake](../../tests/gpu/render_tests.cmake).
