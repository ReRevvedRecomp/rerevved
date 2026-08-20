# Evidence and claims

This repository records behavior recovered from a title image supplied by a
contributor and from the ReXGlue runtime. The title image, extracted assets,
generated guest code, traces, and bulk analysis state are never committed.

## What counts as evidence

Static guest code evidence identifies a durable address or symbol, the tool or
generated artifact used, the observation, and any unresolved ambiguity. Runtime
evidence identifies the build, command, observable result, and boundary tested.

The tool is an instrument, not the authority. Decompiled types, inferred names,
and generated control flow remain interpretations until supported by the guest
instructions, data, or an independent runtime observation. A green build proves
compilation only; runtime claims require a bounded trace, capture, test, or other
independently reviewable result appropriate to the claim.

Repository code, tests, manifests, and validation results establish repository
contracts. Automated output, summaries, and uncited notes are leads, not
evidence.

## Claim boundary

Make the narrowest claim supported by the cited observation. Keep static and
runtime evidence distinct. Do not turn a successful code generation step into a
runtime claim or a forced transition into proof of the title's natural path.

Preserve placeholder names when the evidence does not establish a stable
identity. Record conflicts instead of merging candidates into one assertion.
Hand-placed coordinates, tuning constants, and conspicuously round values are
deliberate until evidence proves otherwise.

## Public boundary

Tracked documentation retains settled conclusions and reproducible methods,
not retail inputs, generated guest code, machine paths, session logs, or private
analysis state. A public claim must carry enough context to be reviewed without
access to those materials.

Preserve source, reference, permission, and provenance citations verbatim,
including dates embedded in external citations.

## Numbers in prose

Keep a number when it carries the claim: an address, offset, size, hash, version
pin, tuning constant, exact tool result, or coverage result. Remove incidental
counts whose omission does not change the sentence's meaning.
