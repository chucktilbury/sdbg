# sdbg

GTK 4 frontend for **GDB** and **LLDB**. It drives the debugger over a PTY (GDB can use MI2), shows source with gutter breakpoints, and includes a separate console for the inferior.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Produces `build/sdbg` and `build/sdbg-dap`.

## Dependencies

Debian/Ubuntu:

```bash
sudo apt install g++ cmake ninja-build pkg-config build-essential \
  libgtk-4-dev libgtkmm-4.0-dev libgtksourceview-5-dev gdb gdbserver lldb
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf \
  gtk4-devel gtkmm4.0-devel gtksourceview5-devel gdb gdb-gdbserver lldb
```

Required pkg-config modules: `gtkmm-4.0`, `gtksourceview-5`.

## Run

```bash
./build/sdbg
./build/sdbg --gdb -- ./a.out -v
./build/sdbg -d lldb --stop-at-entry -b main --run ./app
```

Settings file `debugger.json` is searched in this order:

1. `./debugger.json`
2. `../debugger.json`
3. `~/.config/sdbg/debugger.json`

`--config FILE` overrides. `sdbg-dap` installs next to `sdbg` (not under `~/.config/sdbg/`).

## Layout

- `src/backend.hpp` — GDB/LLDB commands, CLI, JSON config
- `src/session.*` — debugger PTY
- `src/target_pty.*` — inferior console PTY
- `src/app.cpp` — gtkmm 4 UI
- `src/dap_main.cpp` — experimental DAP adapter
- `tests/` — unit tests
