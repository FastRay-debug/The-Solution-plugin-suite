#!/bin/bash
# Builds "The Solution Suite.pkg" - a standard double-click macOS installer.
#
# Usage: packaging/build_mac_pkg.sh <gathered-folder> <version> [output.pkg]
#   <gathered-folder> must contain AU/ VST3/ CLAP/ Standalone/ Skins/
#   (the GitHub Actions build and install_all_plugins.command both make one).
set -euo pipefail

SRC="${1:?gathered plugin folder}"
VERSION="${2:?version}"
OUT="${3:-The Solution Suite.pkg}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ROOT="$WORK/root"
mkdir -p "$ROOT/Library/Audio/Plug-Ins/Components" \
         "$ROOT/Library/Audio/Plug-Ins/VST3/The Solution" \
         "$ROOT/Library/Audio/Plug-Ins/CLAP/The Solution" \
         "$ROOT/Applications/The Solution" \
         "$ROOT/Library/Application Support/Billet Audio/The Solution/Skins"

copy_all() { # src-glob-dir pattern dest
  shopt -s nullglob
  for item in "$1"/$2; do cp -R "$item" "$3/"; done
  shopt -u nullglob
}
copy_all "$SRC/AU"         "*.component" "$ROOT/Library/Audio/Plug-Ins/Components"
copy_all "$SRC/VST3"       "*.vst3"      "$ROOT/Library/Audio/Plug-Ins/VST3/The Solution"
copy_all "$SRC/CLAP"       "*.clap"      "$ROOT/Library/Audio/Plug-Ins/CLAP/The Solution"
copy_all "$SRC/Standalone" "*.app"       "$ROOT/Applications/The Solution"
copy_all "$SRC/Skins"      "*.jpg"       "$ROOT/Library/Application Support/Billet Audio/The Solution/Skins"

# Stop the installer from "relocating" bundles to some other copy it finds on disk.
pkgbuild --analyze --root "$ROOT" "$WORK/components.plist"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i:RootRelativeBundlePath" "$WORK/components.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$WORK/components.plist"
  i=$((i + 1))
done
echo "Packaging $i bundles"

chmod +x "$HERE/macos-scripts/postinstall"
pkgbuild --root "$ROOT" \
         --component-plist "$WORK/components.plist" \
         --identifier com.fastraymedia.thesolution.suite \
         --version "$VERSION" \
         --install-location / \
         --scripts "$HERE/macos-scripts" \
         "$WORK/suite.pkg"

productbuild --package "$WORK/suite.pkg" "$OUT"
echo "Built: $OUT"
