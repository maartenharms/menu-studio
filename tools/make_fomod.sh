#!/usr/bin/env bash
# Build the Menu Studio FOMOD installer zip (the release artifact).
#
# Menu Studio is a single Skyrim SE 1.5.97 DLL, so there is no SE/AE file choice
# and no options; everything installs together and the FOMOD is a branded page
# with a short explanation and an endorse reminder. Once an AE build exists, add
# a step with a type="SelectExactlyOne" group ("Skyrim SE 1.5.97" / "Skyrim AE
# 1.6.x"), each plugin installing its own DLL folder; the rest of this layout is
# reusable.
#
# Package layout (zip root): fomod/{info.xml,ModuleConfig.xml}, Images/, core/
# (everything, always installed).
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

# core: everything, always installed. DLL + INI (verbose logging off for the
# release) + the void meshes/textures (the .png in dist/textures is the dev
# source for the DDS, so leave it out) + docs.
mkdir -p "$STAGE/core/SKSE/Plugins"
cp "$DLL" "$STAGE/core/SKSE/Plugins/"
sed 's/^bVerboseLog=1/bVerboseLog=0/' \
    "$ROOT/dist/SKSE/Plugins/MenuStudio.ini" > "$STAGE/core/SKSE/Plugins/MenuStudio.ini"
cp -r "$ROOT/dist/meshes"   "$STAGE/core/meshes"
cp -r "$ROOT/dist/textures" "$STAGE/core/textures"
find "$STAGE/core/textures" -iname "*.png" -delete
cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/CHANGELOG.md" \
   "$ROOT/THIRD-PARTY-NOTICES.md" "$ROOT/KNOWN-ISSUES.md" "$STAGE/core/"

(cd "$STAGE" && powershell -NoProfile -Command \
    "Compress-Archive -Path * -DestinationPath '$(cygpath -w "$ZIP")' -Force")

echo "packaged FOMOD: $ZIP"
unzip -l "$ZIP"
