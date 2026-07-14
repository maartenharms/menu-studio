#!/usr/bin/env bash
# Build the Menu Studio FOMOD installer zip (the release artifact).
#
# Menu Studio is one universal DLL for Skyrim SE 1.5.97 and Anniversary Edition
# (1.6.1130+), so there is no SE/AE file choice and no options; everything
# installs together and the FOMOD is a branded page with a short explanation and
# an endorse reminder.
#
# Package layout (zip root): fomod/{info.xml,ModuleConfig.xml}, Images/, core/
# (game files, installed to Data), + LICENSE and a short README.txt at the root
# (NOT installed). Docs single-source-of-truth is GitHub; the download carries
# only LICENSE (GPL) + a README.txt pointer, no CHANGELOG/KNOWN-ISSUES/THIRD-PARTY.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="$(sed -n 's/^project(MenuStudio VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
DLL="$ROOT/build/release/MenuStudio.dll"
STAGE="$ROOT/release/fomod-stage"
ZIP="$ROOT/release/MenuStudioSE-$VER.zip"

[ -f "$DLL" ] || { echo "no DLL at $DLL - build first"; exit 1; }

rm -rf "$STAGE" "$ZIP"

# FOMOD metadata + banner (ModuleConfig references Images\menustudio.png).
mkdir -p "$STAGE/fomod" "$STAGE/Images"
cp "$ROOT/fomod/info.xml" "$ROOT/fomod/ModuleConfig.xml" "$STAGE/fomod/"
cp "$ROOT/fomod/banner.png" "$STAGE/Images/menustudio.png"

# core: the game files that install to Data. DLL + INI (verbose logging off for
# the release) + the void meshes/textures (the .png in dist/textures is the dev
# source for the DDS, so leave it out). No docs in core - see below.
mkdir -p "$STAGE/core/SKSE/Plugins"
cp "$DLL" "$STAGE/core/SKSE/Plugins/"
sed 's/^bVerboseLog=1/bVerboseLog=0/' \
    "$ROOT/dist/SKSE/Plugins/MenuStudio.ini" > "$STAGE/core/SKSE/Plugins/MenuStudio.ini"
cp -r "$ROOT/dist/meshes"   "$STAGE/core/meshes"
cp -r "$ROOT/dist/textures" "$STAGE/core/textures"
find "$STAGE/core/textures" -iname "*.png" -delete
# Docs at the ARCHIVE ROOT (not installed to Data). Single-source-of-truth is
# GitHub: ship LICENSE (GPL requires it in the download) + a short README.txt
# pointer only. Full README/CHANGELOG/KNOWN-ISSUES/THIRD-PARTY live on GitHub.
cp "$ROOT/LICENSE" "$STAGE/"
cat > "$STAGE/README.txt" <<'EOF'
Menu Studio
Pause the world when you open a menu and keep your character live and
posed, in a clean studio of your choosing. One download for Skyrim SE
1.5.97 and Anniversary Edition.

Documentation, changelog, source code and issue tracker:
  https://github.com/maartenharms/menu-studio

Licensed under GPL-3.0 (see LICENSE).
EOF

(cd "$STAGE" && powershell -NoProfile -Command \
    "Compress-Archive -Path * -DestinationPath '$(cygpath -w "$ZIP")' -Force")

echo "packaged FOMOD: $ZIP"
unzip -l "$ZIP"
