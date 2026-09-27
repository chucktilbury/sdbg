# Pending editor changes

This branch is for the latest local sdbg work that may not be fully on `main` yet:

- Auto-open source for the loaded target (`info line main` / `image lookup`, sibling `.c`/`.cpp` fallback).
- Highlight current execution line (gutter + full-line background).
- Highlight breakpoints (gutter + line tint); gutter click paints immediately.
- Broader parsing of GDB/LLDB breakpoint listings.

Push `src/app.cpp` and `src/backend.hpp` from the working tree onto this branch before merge if GitHub still has the older copies.
