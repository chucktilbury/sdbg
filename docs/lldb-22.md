# LLDB 22

sdbg looks up `lldb-22` first, then 21…19, 18, `lldb`.

Do not send these (removed / renamed):

- `settings set target.cwd`
- `settings set target.stop-at-entry`
- `settings set target.process.detach-on-fork`

Use:

- `settings set --exists target.launch-working-dir DIR`
- `process launch --stop-at-entry` (or `process launch`)
- `settings set --exists target.process.follow-fork-mode parent|child`
- `settings set --exists target.process.stop-on-fork true|false`

`--exists` (`-e`) skips unknown paths instead of `error: invalid value path`.

Rebuild from the working tree (`src/backend.hpp`) — an older `sdbg` binary still emits the removed names.
