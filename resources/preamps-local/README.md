<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Local preamp NAM drop zone

Private / experimental `.nam` preamp captures go **here**, not in `../preamps/`.

- CMake globs this folder too (`CONFIGURE_DEPENDS`), so anything dropped in is **embedded on the next
  build** exactly like a factory preamp — same naming convention as `../preamps/README.md`.
- Everything here except this README is **gitignored** (`resources/preamps-local/*.nam`), so local
  captures are **never committed** and never ship.

Use `../preamps/` only for the tracked, shipped factory set.
