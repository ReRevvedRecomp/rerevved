# Style guide

This guide covers authored title code, public headers, build files, and tools.
It complements the [contribution contract](../CONTRIBUTING.md). The
[comment policy](ai_agents/comments-and-prose.md) owns source comments and
public prose; [evidence and claims](ai_agents/evidence-and-claims.md) owns
behavioral claims and provenance.

## Scope and structure

- Keep changes local to the feature being maintained. Prefer a direct
  implementation before introducing a shared abstraction.
- Put public mod interfaces in `api/` and their title implementations in
  `src/`. Follow the existing feature's file family: public header, registry,
  implementation, and hooks have different responsibilities.
- Keep guest bridges narrow. Hook addresses, register lists, field offsets,
  IDs, and tuning values are behavior, not style.
- Edit the inputs to generated code. Do not reformat generated guest code,
  installed SDK files, or third-party code as part of title cleanup.

## C and C++

The title builds as C++23. Public headers also support C consumers; keep C++
types and ownership mechanisms behind the C ABI. The
[mod API guide](modding-api.md) and individual public headers define those
contracts.

### Formatting

Use [`.clang-format`](../.clang-format) with clang-format 22 for mechanical
formatting. Keep this guide and the configuration consistent when changing a
style choice. `Standard: Latest` in the formatter does not change the compiler
language level in [CMakeLists.txt](../CMakeLists.txt).

- Use Allman braces, four spaces, and no tabs. Brace control-flow bodies.
- Use `Type* pointer`, `const Type* pointer`, and `Type& reference`.
- Keep one statement per line and use early returns where they clarify error
  handling.
- Keep include sorting and declaration spacing under formatter control.
- Align consecutive declarations, assignments, enum values, macros, and
  trailing comments with the formatter. Declaration alignment also applies
  to parameter names in multiline function-pointer declarations.
- There is no fixed column limit. Break long signatures at meaningful
  boundaries, using one parameter per line for multiline declarations. Do not
  shorten meaningful public names just to fit a line.

```cpp
if (!IsReady())
{
    return false;
}

typedef int32_t (*ReRevvedGetUnitMovementRuleFn)(
    uint32_t                      index,
    ReRevvedUnitMovementRuleInfo* out,
    uint32_t                      out_size);
```

Put short comments about individual enum values, structure fields, and list
entries inline. Keep shared invariants above the group and longer contracts
in the relevant public header or owning guide, as specified by the comment
policy.

### Naming and headers

- Use `lower_snake_case` filenames, such as `main_menu_logo_asset.h`.
- Use `UpperCamelCase` for types, scoped enum values, and namespaced functions;
  `lowerCamelCase` for variables, parameters, fields, and private helpers; and
  `kLowerCamelCase` for internal constants. Plain enum values use
  `UPPER_SNAKE_CASE`.
- Organize internal C++ under `rerevved` and its feature namespaces. Type and
  member names describe their role without repeating the product or enclosing
  enum name: `TextSurface::LeaderName`, for example. Variables and parameters
  do not need a product prefix.
- Public C types and exports use the `ReRevved` prefix. Public C macros and
  unscoped enumerators use `REREVVED_` with uppercase underscore-separated
  words because C has no namespaces. Keep these names when using the public
  API from C++; do not add aliases solely to hide the prefix.
- Name APIs by their supported feature or screen. Keep public identifiers,
  numeric values, and mirrored headers coordinated under the API contract.
- Use `#pragma once` and include a header's direct dependencies. Keep private
  helpers out of public headers unless callers need them.

### Interface exceptions

Public C API names and member spellings, SDK overrides, generated hook symbols,
and external entry points follow their interface contracts. Preserve names
resolved from configuration or by other binaries. Generated global hooks keep
their distinguishing prefix; their internal helpers follow the C++ rules above.
Serialized keys, package IDs, and guest identifiers follow their owning schemas.
The C++23 language level and C-compatible public headers remain title build and
ABI requirements.

### Ownership and errors

Prefer explicit ownership and standard-library value types in host code.
Use fixed-width types where width or signedness is part of an ABI or guest
layout. Use `auto` when the initializer makes the type and ownership clear.

Use `static_cast` for intentional value conversions and `dynamic_cast` for
checked downcasts of polymorphic C++ objects. Do not use C-style casts or
substitute `static_cast` for a checked downcast. Guest address translation and
platform function-pointer resolution require their existing boundary-specific
handling; do not substitute generic casts as a style cleanup. Validate foreign
inputs before reading or copying them. Preserve documented error ordering,
output clearing, fallback behavior, and registration order. Exceptions must not
escape a public C ABI boundary.

## Tools and data

- Python uses four spaces, `snake_case` functions and variables, and
  `UPPER_SNAKE_CASE` constants. Follow existing standard-library tooling and
  keep imports explicit.
- PowerShell uses the existing Verb-Noun helper names, script parameters, and
  error-handling conventions. Quote literal paths and keep destructive file
  operations bounded to their intended output directory.
- Keep CMake changes target-scoped. Follow the existing target names and
  source lists rather than introducing global flags for one feature.
- Preserve JSON/TOML keys, manifest schemas, file encodings, source locators,
  and meaningful record order. Data changes require their own evidence.

## Verification

From the repository root, run `scripts\verify.ps1` for repository checks.
The clang-format gate checks authored C/C++ under `api/`, `src/`, and `tests/`.
Public-header changes must preserve the C contract and update the
Mods mirror when applicable.

The [runtime guide](rexglue-runtime.md) owns build commands. After a native
build, run the CTest suite registered in that build directory. Behavior changes
need tests that exercise their contract; a formatting-only change can instead
be checked for unchanged code tokens. Always run `git diff --check` and inspect
the complete diff for unintended edits.
