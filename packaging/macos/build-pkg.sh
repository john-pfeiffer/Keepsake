#!/usr/bin/env bash
#
# Builds the macOS installer: one component pkg per plugin format, wrapped in a
# single distribution pkg (spec §7).
#
#   build-pkg.sh <version> <artefacts-dir> <out-dir>
#
# <artefacts-dir> is the CMake output, e.g. build/Keepsake_artefacts/Release.
#
# Signing and notarization are OPTIONAL and driven entirely by the environment
# (spec: CI holds certs as encrypted secrets; without them this still produces
# an unsigned pkg for right-click-open testing):
#
#   MACOS_SIGN_IDENTITY_APP        "Developer ID Application: ..." for codesign
#   MACOS_SIGN_IDENTITY_INSTALLER  "Developer ID Installer: ..." for productbuild
#   NOTARY_APPLE_ID / NOTARY_TEAM_ID / NOTARY_PASSWORD   for notarytool + staple
#
set -euo pipefail

VERSION="${1:?usage: build-pkg.sh <version> <artefacts-dir> <out-dir>}"
ARTEFACTS="${2:?missing artefacts dir}"
OUT="${3:?missing out dir}"

IDENTIFIER_BASE="com.elanvitalstudios.keepsake"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$OUT"

sign_bundle() {
    if [ -n "${MACOS_SIGN_IDENTITY_APP:-}" ]; then
        echo "Signing $1"
        codesign --force --options runtime --timestamp \
                 --sign "$MACOS_SIGN_IDENTITY_APP" "$1"
        codesign --verify --strict "$1"
    else
        echo "No MACOS_SIGN_IDENTITY_APP - leaving $1 unsigned"
    fi
}

# --- Stage + sign each format at its system install location -----------------
declare -a COMPONENT_PKGS=()

build_component() { # <bundle-path> <install-location> <component-name>
    local bundle="$1" location="$2" name="$3"
    local root="$STAGE/root-$name"

    [ -e "$bundle" ] || { echo "Missing artefact: $bundle" >&2; exit 1; }

    mkdir -p "$root$location"
    cp -R "$bundle" "$root$location/"
    sign_bundle "$root$location/$(basename "$bundle")"

    local pkg="$STAGE/Keepsake-$name.pkg"
    pkgbuild --root "$root" \
             --identifier "$IDENTIFIER_BASE.$name" \
             --version "$VERSION" \
             --install-location / \
             "$pkg"
    COMPONENT_PKGS+=("--package" "$pkg")
}

build_component "$ARTEFACTS/VST3/Keepsake.vst3"       "/Library/Audio/Plug-Ins/VST3"       vst3
build_component "$ARTEFACTS/AU/Keepsake.component"    "/Library/Audio/Plug-Ins/Components" au
build_component "$ARTEFACTS/Standalone/Keepsake.app"  "/Applications"                      app

# --- Wrap into one distribution pkg ------------------------------------------
FINAL="$OUT/Keepsake-$VERSION-macOS.pkg"

if [ -n "${MACOS_SIGN_IDENTITY_INSTALLER:-}" ]; then
    productbuild "${COMPONENT_PKGS[@]}" --sign "$MACOS_SIGN_IDENTITY_INSTALLER" "$FINAL"
else
    echo "No MACOS_SIGN_IDENTITY_INSTALLER - building unsigned installer"
    productbuild "${COMPONENT_PKGS[@]}" "$FINAL"
fi

# --- Notarize + staple (only meaningful for a signed pkg) --------------------
if [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${MACOS_SIGN_IDENTITY_INSTALLER:-}" ]; then
    echo "Notarizing $FINAL"
    xcrun notarytool submit "$FINAL" \
        --apple-id "$NOTARY_APPLE_ID" \
        --team-id "$NOTARY_TEAM_ID" \
        --password "$NOTARY_PASSWORD" \
        --wait
    xcrun stapler staple "$FINAL"
else
    echo "Notarization credentials absent - installer is NOT notarized"
fi

echo "Built $FINAL"
