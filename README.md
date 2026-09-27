# sdbg

GTK 4 frontend for **GDB** and **LLDB**. It drives the debugger over a PTY (GDB can use MI2), shows source with gutter breakpoints, and includes a separate console for the inferior.

## Features

- Switch engine: LLDB or GDB
- Load / run / reload target without restarting the debugger
- Auto-open source for the target (`main` from debug info, sibling `.c`/`.cpp` fallback)
- Current-line and breakpoint highlighting (gutter + full-line background)
- Target arguments, cwd, environment, setup commands, stop-at-entry
- Inferior I/O: stdin/stdout/stderr files, TTY, optional target PTY console
- Remote (`gdbserver` / `gdb-remote`), attach by PID, core dumps
- Follow-fork / detach-on-fork
- Multi-tab source view (GtkSourceView), find, go-to-line, reload on disk change
- Stack, threads, locals, registers, watches, memory dump
- Hex/dec locals toggle, register edit, source-map / `substitute-path`
- Debugger console with command history; non-blocking continue/run
- Dark/light theme, font size, saved window size
- CLI + `debugger.json` persistence (`./`, `../`, then `~/.config/sdbg/`)

## External dependencies

### Debian / Ubuntu

```bash
sudo apt install g++ cmake ninja-build pkg-config build-essential \
  libgtk-4-dev libgtkmm-4.0-dev libgtksourceview-5-dev gdb gdbserver lldb
```

### Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf \
  gtk4-devel gtkmm4.0-devel gtksourceview5-devel gdb gdb-gdbserver lldb
```

Required pkg-config modules: `gtkmm-4.0`, `gtksourceview-5`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Produces `build/sdbg` and `build/sdbg-dap`.

## Run

```bash
./build/sdbg
./build/sdbg --gdb -- ./a.out -v
./build/sdbg -d lldb --stop-at-entry -b main --run ./app
```

Config search order: `./debugger.json`, `../debugger.json`, `~/.config/sdbg/debugger.json`.

## License

Use and modify freely.
