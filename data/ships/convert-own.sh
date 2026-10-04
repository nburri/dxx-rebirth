#!/bin/sh
# How the fork's own ships were made: procedural low-poly models (CC0 1.0,
# no third-party content), their glTF sources in src/.
# Usage: data/ships/convert-own.sh <shipconv>
set -eu
shipconv=$1
out=$(dirname "$0")
own() {
	"$shipconv" "$out/src/$1.glb" -o "$out/$1.dxship" --name "$1" --title "$2" \
		--author "Claude (procedural)" --licence CC0-1.0 \
		--source "dxx-rebirth fork, procedural (shipgen)" --description "$3"
}
own anvil Anvil "Anvil: armoured box hull, wedge nose, side sponsons with their own engines, short stubby wings, twin canted tail fins."
own manta Manta "Manta: wide flat lifting body (rounded delta), raised canopy, dorsal engine block, two buried nacelles, twin fins."
own locust Locust "Locust: fat octagonal fuselage; stub wings to vertical armour plates, each with two big engine pods; H-shaped from the front."
own bulwark Bulwark "Bulwark: faceted wedge pod with a slit visor, two big swept trapezoid shield panels raised in a wide V on short pylons."
