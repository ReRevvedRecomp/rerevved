# Comments and prose

Deletion is the default for comments in host code and driver scripts.
Keep a comment only when it records a current invariant, a guest or host ABI contract,
a client quirk, a safety constraint, a source citation, or where a manually placed value came from.

Keep source attributions verbatim. Keep PM4, packet, DAT, wiki, and other source
locators verbatim. These identifiers are provenance, not work-log prose.

Compress a survivor to one to three lines at the use site. Move a longer
contract to a declaration or the relevant technical guide and leave a short
pointer. Delete dates, phase labels, wall numbers, capture set identifiers,
experiment narratives, and reports of earlier fixes or diagnostics.

Generated comments are generated output. Preserve them or update their owning
generator and regenerate. Do not edit generated PPC, switch-table, ReXGlue
output, or progress files during a prose pass.

Runtime strings, command help, manifest fields, workflow data, and self-test
labels are behavior or data. Do not rewrite them as if they were comments.

## Authored public prose

Public prose includes the README, CONTRIBUTING, the docs index, and any page
a stranger reads. Use a plain, direct register.

The [public contribution policy](README.md) applies to all tracked Markdown and
structured data.

- Avoid over-hyphenation. Keep hyphens in established technical terms, but
  prefer ordinary noun phrases to invented compound modifiers or long chains.
- Use semicolons sparingly. Prefer a period, comma, or short list unless a
  semicolon makes two closely related clauses materially clearer.
- Remove parenthetical asides. Make important context a short sentence. Delete
  the rest.
- Use short declarative sentences with one idea each. Give a rule one line
  of practical justification.
- "Footgun" and "load-bearing" never ship in docs or comments. Name the
  actual hazard or dependency: "pitfall guard", "required order",
  "X relies on Y".

Internal working notes are outside this public policy tier.
