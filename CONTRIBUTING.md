# Contributing

Bug reports, ideas, documentation fixes and code are all welcome. A change is
judged on what it does and how well it fits the project, not on who or what
wrote it.

## Ways to help

- **Report a bug.** Open a
  [bug report](https://github.com/ReRevvedRecomp/rerevved/issues/new?template=bug_report.md)
  or ask on [Discord](https://discord.gg/j6VttvdnMD). [Support](docs/support.md)
  lists the details to include and how to share a log safely.
- **Suggest a feature.** Open a
  [feature request](https://github.com/ReRevvedRecomp/rerevved/issues/new?template=feature_request.md).
  Say what problem it solves. Add any guest addresses, function names or save
  layout notes you already have.
- **Fix the docs.** Small corrections can go straight to a pull request.
- **Send code.** For a large or risky change, ask on Discord or open an issue
  first. Agreeing on the approach early saves a rewrite.

## Pull requests

Open pull requests against `develop`. Keep each one to a single concern.
Unrelated cleanup belongs in its own pull request.

Before you open one:

1. Build with the SDK revision in
   [`rexglue-sdk.lock.json`](rexglue-sdk.lock.json). SDK changes go to the
   [SDK repository](https://github.com/ReRevvedRecomp/rerevved-sdk) first and
   need their own checks before the title repins.
2. Add a focused regression check for changed behavior. See
   [Test authoring](docs/ai_agents/test-authoring.md).
3. Run `.\scripts\verify.ps1`. After a native build, also run the CTest suite
   in that build directory. [Development](docs/development.md#checks-and-tests)
   has the commands.
4. For documentation, check heading anchors by hand and run `git diff --check`.
   The verify script checks file links and ASCII but not anchors.

In the description, explain what the change does. If it closes an issue, write
`Closes #123` so GitHub links them.

### What not to submit

- Retail game files, extracted assets, or anything else from your game copy.
- Generated guest code. Change the inputs to code generation instead.
- Build output, logs, saves, credentials, or local machine paths.
- Changes to third-party code or installed SDK files.

### Code expectations

Follow the [style guide](docs/style-guide.md). Preserve guest control flow
unless a feature needs a documented compatibility hook. Keep diagnostics off by
default.

## AI-assisted contributions

AI-assisted work is reviewed exactly like any other work. If the change is
correct, tested and in line with the project, it is welcome however it was
written. You are its author: be ready to explain any line of it.

Coding agents follow [the agent guide](docs/ai_agents/README.md).

## What slows a review down

Reviewers ask for these to be fixed before merging:

- Non-ASCII prose, such as em dashes and curly quotes.
- Comments that narrate history, such as "this used to do X, now it does Y".
  Git keeps that. [Comments and prose](docs/ai_agents/comments-and-prose.md)
  covers what a comment should record.
- AI tools listed as co-authors, or "generated with" lines in commits or pull
  requests.
- Unrelated reformatting mixed into a functional change.

## Commit messages

Subjects use `type: imperative summary`, at most 50 characters, with no body or
parentheses. Use a lowercase type and a lowercase opening verb. Keep proper
nouns, acronyms and identifiers as written. Types include `build`, `chore`,
`ci`, `docs`, `feat`, `fix`, `resources`, `test`, and `tools`.

```text
fix: keep the camera on the selected unit
```
