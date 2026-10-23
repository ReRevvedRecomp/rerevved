# Guidance for coding agents

Read these rules before making automated changes. The
[documentation index](../README.md) separates player guides from development
and technical references.

Read the [style guide](../style-guide.md) and
[Development](../development.md) before editing. These pages
own branch, commit, verification, and review rules. Use the exact SDK lock; changes to another
repository require their own scope and verification.

Keep retail inputs, generated guest code, credentials, machine paths, session
notes, and private repository references out of tracked files. Change generated
inputs rather than outputs. Preserve unrelated work and source attribution.

Each claim needs a useful source locator and a clear limit. A build proves
compilation, not gameplay or visual correctness. Read
[Evidence and claims](evidence-and-claims.md) and
[Comments and prose](comments-and-prose.md) for those boundaries. Read
[Test authoring](test-authoring.md) before adding or changing tests.
