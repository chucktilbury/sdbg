# Current LLDB setting names used by sdbg

Replace obsolete paths that current LLDB rejects:

- `target.cwd` → `target.launch-working-dir`
- `target.stop-at-entry` → `process launch --stop-at-entry` (not a setting)
- `target.process.detach-on-fork` → `target.process.follow-fork-mode` + `target.process.stop-on-fork`

Apply these edits in `src/backend.hpp` (`apply_params`, `run_cmd`, `fork_cmds`) if that file on the branch is still the older copy.
