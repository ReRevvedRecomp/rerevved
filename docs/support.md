# Support

Report title runtime, game compatibility, graphics, input, or save problems in
the [ReRevved issue tracker](https://github.com/ReRevvedRecomp/rerevved/issues).
Include clear reproduction steps and the build, operating system, CPU, and GPU
details requested by the bug form.

For a storage-profile issue, include the profile ID and whether the
default-to-profile copy was requested. The [storage profile contract](rexglue-runtime.md#storage-profiles)
defines default `P=B` behavior, profile-local state, and shared marketplace
content.

## Sharing a log

For a crash or freeze, the latest default-profile packaged-player log is
normally `Documents\rerevved\logs\rerevved_NNN.log` on Windows. Without an
explicit `log_file` override, a named profile stores its log under
`Documents\rerevved\profiles\<id>\logs`.
On Linux, the default-profile log is under
`$XDG_DATA_HOME/rerevved/logs`, or `~/.local/share/rerevved/logs` when
`XDG_DATA_HOME` is not set.

Before uploading a log:

1. Make a copy outside the runtime log folder, then open the copy in a text
   editor and review the complete file.
2. Remove or replace personal information, secrets, usernames, hostnames, and
   absolute paths. Keep the error and nearby diagnostic context intact.
3. Attach only the reviewed copy. Do not upload configuration files, saves,
   retail game files, extracted assets, or the complete ReRevved data folder.
4. If the log cannot be shared safely, submit the reproduction and build
   details first. A maintainer can request a smaller relevant excerpt.
