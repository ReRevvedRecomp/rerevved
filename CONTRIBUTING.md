# Contributing

ReRevved accepts focused changes that improve the ReXGlue title build,
runtime correctness, documentation, or supporting analysis tools. Do not submit
copyrighted game files, extracted assets, generated guest code, build output, or
private evidence files.

## Before a change

If automated or AI assistance is used, read
[`docs/ai_agents/README.md`](docs/ai_agents/README.md) for the contribution and
evidence policies. Base runtime work on the SDK commit pinned by
[`rexglue-sdk.lock.json`](rexglue-sdk.lock.json). Keep
the sibling SDK checkout read-only unless the change is explicitly scoped to
that repository.

## Commit subjects

Use `type: imperative summary` and keep the complete subject at 50 characters
or fewer. Choose a type that matches the change, such as `build:`, `chore:`,
`ci:`, `docs:`, `feat:`, `fix:`, `resources:`, `test:`, or `tools:`. A bare
summary without a type is not accepted.

## Change boundaries

- Preserve the title's original control flow unless the documented contract
  requires a compatibility hook.
- Keep diagnostics disabled by default and preserve the original behavior when
  enabled.
- Put settled public facts in the canonical tracked guide. Keep machine paths,
  experiments, and private evidence out of tracked files.
- Do not edit generated files directly.
