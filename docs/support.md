# Support

Report title runtime, game compatibility, graphics, input, or save problems on
[Discord](https://discord.gg/j6VttvdnMD). Include clear reproduction steps and
the build, operating system, CPU, and GPU details.

For an issue with a storage profile, include the profile ID and say whether you
requested a copy from the default profile. The
[storage profile contract](rexglue-runtime.md#storage-profiles) explains which
state belongs to each profile and which marketplace content is shared.

## Sharing a log

For a crash or freeze, use **F1 > Gameplay > Support > Open Log Folder** to
find the active log. This follows named profiles and explicit log overrides.
See [Configuration](configuration.md#file-locations) for default paths.
If the game cannot open, look for the latest `rerevved_NNN.log` in that
profile's `logs/` folder.

On Windows, after an unhandled exception, a crash dump named like the log
(`rerevved_NNN.dmp`) may sit beside it. It is a small minidump of thread stacks
and the module list. It may contain memory contents, so attach it only when a
maintainer requests it.

Before uploading a log:

1. Make a copy outside the runtime log folder, then open the copy in a text
   editor and review the complete file.
2. Remove or replace personal information, secrets, usernames, hostnames, and
   absolute paths. Keep the error and nearby diagnostic context intact.
3. Attach only the reviewed copy. Do not upload configuration files, saves,
   retail game files, extracted assets, or the complete ReRevved data folder.
4. If the log cannot be shared safely, submit the reproduction and build
   details first. A maintainer can request a smaller relevant excerpt.
