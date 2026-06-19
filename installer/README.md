# Installers & release upload

Packaging that lands the plugin in the right folders, plus the upload to the download
server. Built by `.github/workflows/release.yml` (tag `vX.Y.Z` or run it manually).

| Platform | Artifact | Installs to |
|----------|----------|-------------|
| macOS | `OrbitCab-<ver>-macOS.pkg` (`build-pkg.sh`) | `/Library/Audio/Plug-Ins/{VST3,Components}` |
| macOS | `OrbitCab-<ver>-macOS.zip` | (manual copy — VST3 + AU bundles) |
| Windows | `OrbitCab-<ver>-Windows-Setup.exe` (`orbitcab.iss`, Inno Setup 6) | `Common Files\VST3\OrbitCab.vst3` + `Program Files\OrbitCab\OrbitCab.exe` |
| Windows | `OrbitCab-<ver>-Windows.zip` | (manual copy — VST3 + Standalone) |

All four land on the download server under `OrbitCab/<version>/`, via `rsync` over SSH.

## Signing status
- **macOS** — ✅ **signed (Developer ID) + notarized + stapled**. The `.pkg`
  installs cleanly — no right-click→Open, no quarantine clearing. Done on the self-hosted
  Mac: certs in the login Keychain, a stored notarytool profile, and a one-time
  `security set-key-partition-list` so `codesign`/`productsign` run without prompts.
- **Windows** — still **unsigned**: SmartScreen warns ("Windows protected your PC" →
  **More info → Run anyway**) until an OV/EV (or Azure Trusted Signing) cert is added.

## Required repo secrets/vars (for signing + the upload step)
Add in **repo Settings → Secrets and variables → Actions**:
- `SSH_PRIVATE_KEY` — a private key authorized for the download-server host
- `KNOWN_HOSTS` — `ssh-keyscan` output for the download-server host
- `DEPLOY_HOST` — `user@host` of the download server
- `MACOS_APP_IDENTITY` / `MACOS_INSTALLER_IDENTITY` — Developer ID signing identities
- `NOTARY_PROFILE` (variable) — the notarytool keychain-profile name

Without them the build/package jobs still run; only the `publish` (upload) job fails.
