<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Poweramp NAM models (local-only)

This folder holds the **Neural Amp Modeler (`.nam`) poweramp captures** that the build embeds as
**factory** models for the editor's **POWERAMP** stage (input → amp → cab).

**The `.nam` files here are gitignored — they are NOT in the public repo, on purpose.** The amp-stage
*code* (`cab::AmpStage`, NAM core, the UI, rate-match) is open-source AGPL and ships freely. The model
*content* does not: captures can be of third-party / commercial gear, so — like the official Neural Amp
Modeler plugin — the open-source build ships **without** bundled captures. You supply your own locally.

A fresh clone builds fine with this folder empty; the POWERAMP feature simply hides itself when there
are no models to load.

## Two sources, one selector

The POWERAMP selector merges two libraries:

* **Factory** — `.nam` files in *this* folder, embedded into the build (what this README is about).
* **User** — a per-machine, system-wide folder shared by every plugin instance, managed from the gear
  panel (Add… / Remove). On macOS: `~/Library/Application Support/Darwin's Cat/OrbitCab/Poweramps/`.

## Naming → mode + tubes

The filename's display name and its **mode** (the PP / SE / Other switch) come from a whole-word
`PP` or `SE` token, which is dropped from the shown name:

```
6L6 PP.nam          → mode PP  (push-pull, a pair of tubes → 2 glowing tubes),  shown "6L6"
6L6 SE.nam          → mode SE  (single-ended, one tube      → 1 glowing tube),   shown "6L6"
Marshall Plexi.nam  → mode Other (no tag → no tubes, amp icon only),             shown "Marshall Plexi"
```

So within a mode the row reads like plain tube buttons (`6L6`, `EL34`, …); the PP/SE distinction is the
mode switch, not part of each label. The number of glowing tubes follows the mode (PP → 2, SE → 1,
Other → 0) — there's no separate tube-count to set. A legacy trailing `-1`/`-2` count is tolerated and
stripped (`6L6 PP-2.nam` still shows "6L6"), so old captures display cleanly without a rename.

Drop files here and rebuild — CMake re-globs this folder (`CONFIGURE_DEPENDS`) and embeds whatever's
present. All factory captures are 48 kHz (the rate-matcher runs them at native rate on any host SR).

Capture your own tube poweramp, or grab models you have the right to use (e.g. tone3000.com). Whatever
you put here stays on your machine — it never leaves in a commit.
