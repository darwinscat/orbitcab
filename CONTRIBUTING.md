# Contributing to OrbitCab

Thanks for your interest. OrbitCab is **GNU AGPLv3-or-later** (see
[`LICENSE`](LICENSE)).

## Status

This is currently a solo project. Issues and small PRs are welcome, but the
**contributor-license policy is not yet finalized**: whether contributions are accepted
as plain *inbound = outbound* AGPLv3, under a DCO sign-off, or under a CLA that grants
relicensing rights. **Until that's decided, please open an issue to discuss before
sending a non-trivial PR** so your work isn't blocked on a policy we haven't written.

## Ground rules for code

- **Every source file needs the SPDX header:**
  ```
  // SPDX-License-Identifier: AGPL-3.0-or-later
  // Copyright (c) <year> <you>. Part of OrbitCab — see LICENSE.
  ```
- **New third-party dependencies must be AGPL-compatible** (BSD / MIT / Apache-2.0,
  etc.) and recorded in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). No
  GPL-incompatible or stricter-copyleft code.
- **🔴 Real-time audio rule:** nothing in `processBlock` / `process()` (or anything
  they call) may allocate, lock, do I/O, or throw. Preallocate in `prepare*`; cross
  threads with atomics / lock-free FIFOs. See
  [`docs/CPP-REFRESHER.md`](docs/CPP-REFRESHER.md).
- **State is versioned and back-compatible** — don't break existing DAW sessions
  (`stateVersion`, frozen param IDs; see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)).
- Match the surrounding style ([`docs/CODING-STYLE.md`](docs/CODING-STYLE.md) /
  `.clang-format`). Keep the build green: build + `auval` + `pluginval @10` +
  `OrbitCab_Tests` + `orbitcab_dsp_test`.

The **OrbitCab** and **Darwin's Cat** names and logos are trademarks and are *not*
covered by the code license.
