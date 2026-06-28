#!/usr/bin/env bash
# Build a macOS installer package (.pkg): the VST3 + AU + CLAP plug-ins into the system plug-in
# folders, and the Standalone app into /Applications. This produces an UNSIGNED .pkg — release.yml
# then productsigns it (Developer ID Installer) + notarizes + staples. The bundles it copies are
# expected to be already codesigned (release.yml signs them before calling this).
#
# Usage: build-pkg.sh <version> <release-artefacts-dir> <out-dir>
#   e.g. installer/build-pkg.sh 1.0.0 build/OrbitCab_artefacts/Release dist
set -euo pipefail

VERSION="${1:?usage: build-pkg.sh <version> <release-dir> <out-dir>}"
REL="${2:?missing release artefacts dir}"
OUT="${3:?missing output dir}"

# Stage the FULL absolute install tree and use --install-location "/" so plug-ins and the app can
# land in DIFFERENT roots from one pkgbuild: plug-ins under /Library/Audio/Plug-Ins, the Standalone
# under /Applications.
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
PLUG="$stage/Library/Audio/Plug-Ins"
mkdir -p "$PLUG/VST3" "$PLUG/Components" "$stage/Applications" "$OUT"

cp -R "$REL/VST3/OrbitCab.vst3"      "$PLUG/VST3/"
cp -R "$REL/AU/OrbitCab.component"   "$PLUG/Components/"
# CLAP is a bundle dir on macOS, like VST3 — lands in /Library/Audio/Plug-Ins/CLAP.
if [ -d "$REL/CLAP/OrbitCab.clap" ]; then
  mkdir -p "$PLUG/CLAP"
  cp -R "$REL/CLAP/OrbitCab.clap"    "$PLUG/CLAP/"
fi
# Standalone app → /Applications (already signed + stapled by release.yml).
if [ -d "$REL/Standalone/OrbitCab.app" ]; then
  cp -R "$REL/Standalone/OrbitCab.app" "$stage/Applications/"
fi

pkgbuild \
  --root "$stage" \
  --install-location "/" \
  --identifier "com.darwinscat.orbitcab" \
  --version "$VERSION" \
  "$OUT/OrbitCab-$VERSION-macOS.pkg"

echo "built $OUT/OrbitCab-$VERSION-macOS.pkg"
