#!/bin/bash
set -eux -o pipefail

if ! [[ -v rebirth_uname_m ]]; then
	rebirth_uname_m="$(uname -m)"
fi
appimage="linuxdeploy-$rebirth_uname_m.AppImage"

# Grab AppImage package at specific version
curl \
	--fail \
    --silent \
    --show-error \
    --location \
    --output "#1" \
	https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/"{$appimage}" \
    || exit 3
chmod a+x "$appimage"

build_appimage() {
    name="$1"
    prettyname="$2"

    # Copy gamecontrollerdb.txt into AppDir if available
    if [ -f "contrib/gamecontrollerdb.txt" ]; then
        mkdir -p "${name}.appdir/usr/share/${name}"
        cp --link "contrib/gamecontrollerdb.txt" "${name}.appdir/usr/share/${name}/"
    fi

    # The bundled custom ships (data/ships/README.md), in the share path
    mkdir -p "${name}.appdir/usr/share/${name}/ships"
    cp --link data/ships/*.dxship data/ships/README.md "${name}.appdir/usr/share/${name}/ships/"
    # The bundled missions (data/missions/README.md), in the share path
    mkdir -p "${name}.appdir/usr/share/${name}/missions"
    cp --link data/missions/*.HOG data/missions/*.MN2 data/missions/README.md "${name}.appdir/usr/share/${name}/missions/"
    # The bundled sounds (data/sounds/README.md), in the share path
    mkdir -p "${name}.appdir/usr/share/${name}/sounds"
    cp --link data/sounds/*.wav data/sounds/README.md "${name}.appdir/usr/share/${name}/sounds/"

    # Package!
    OUTPUT="${prettyname}.AppImage"	\
    "./$appimage" \
        --output appimage \
        --appdir="${name}.appdir" \
        --executable="build/${name}/${name}" \
        --desktop-file="${name}/${name}.desktop" \
        --icon-file="${name}/${name}.png"
}

# Build each app
build_appimage "d2x-rebirth" "D2X-Rebirth"
