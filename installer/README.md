# Installers & release upload

Packaging that lands the plugin in the right folders, plus the upload to the download
server. Built by `.github/workflows/release.yml` (tag `vX.Y.Z` or run it manually).

| Platform | Artifact | Installs to |
|----------|----------|-------------|
| macOS | `OrbitCab-<ver>-macOS.pkg` (`build-pkg.sh`) | `/Library/Audio/Plug-Ins/{VST3,Components,CLAP}` |
| macOS | `OrbitCab-<ver>-macOS.zip` | (manual copy — VST3 + AU + CLAP bundles) |
| Windows | `OrbitCab-<ver>-Windows-Setup.exe` (`orbitcab.iss`, Inno Setup 6) | `Common Files\{VST3\OrbitCab.vst3, CLAP\OrbitCab.clap}` + `Program Files\OrbitCab\OrbitCab.exe` |
| Windows | `OrbitCab-<ver>-Windows.zip` | (manual copy — VST3 + CLAP + Standalone) |

On a version tag (or a manual run with `dry_run=false`), `release.yml` attaches these to a
**GitHub Release** — GitHub hosts the binaries; there's no own download server.

## Signing status
- **macOS** — ✅ **signed (Developer ID) + notarized + stapled**. The `.pkg`
  installs cleanly — no right-click→Open, no quarantine clearing. Done on the self-hosted
  Mac: certs in the login Keychain, a stored notarytool profile, and a one-time
  `security set-key-partition-list` so `codesign`/`productsign` run without prompts.
- **Windows** — still **unsigned**: SmartScreen warns ("Windows protected your PC" →
  **More info → Run anyway**) until an OV/EV (or Azure Trusted Signing) cert is added.

## Required repo secrets/vars (signing only)
Publishing uses the built-in `GITHUB_TOKEN`, so only the macOS signing bits are needed.
Add in **repo Settings → Secrets and variables → Actions**:
- `MACOS_APP_IDENTITY` / `MACOS_INSTALLER_IDENTITY` (secrets) — Developer ID signing identities
- `NOTARY_PROFILE` (variable) — the notarytool keychain-profile name

(The old `SSH_PRIVATE_KEY` / `KNOWN_HOSTS` / `DEPLOY_HOST` deploy secrets are no longer used —
binaries go to GitHub Releases, not an SSH'd server.)

Without them the build/package jobs still run; only the `publish` (upload) job fails.
