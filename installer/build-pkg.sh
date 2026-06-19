#!/usr/bin/env bash
# Build a macOS installer package (.pkg) that drops the VST3 + AU into the system
# plug-in folders. This produces an UNSIGNED .pkg — release.yml then productsigns it
# (Developer ID Installer) + notarizes + staples.
#
# Usage: build-pkg.sh <version> <release-artefacts-dir> <out-dir>
#   e.g. installer/build-pkg.sh 1.0.0 build/OrbitCab_artefacts/Release dist
set -euo pipefail

VERSION="${1:?usage: build-pkg.sh <version> <release-dir> <out-dir>}"
REL="${2:?missing release artefacts dir}"
OUT="${3:?missing output dir}"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/VST3" "$stage/Components" "$OUT"

cp -R "$REL/VST3/OrbitCab.vst3"      "$stage/VST3/"
cp -R "$REL/AU/OrbitCab.component"   "$stage/Components/"

# --root mirrors the install tree; --install-location is where it lands.
pkgbuild \
  --root "$stage" \
  --install-location "/Library/Audio/Plug-Ins" \
  --identifier "com.darwinscat.orbitcab" \
  --version "$VERSION" \
  "$OUT/OrbitCab-$VERSION-macOS.pkg"

echo "built $OUT/OrbitCab-$VERSION-macOS.pkg"
