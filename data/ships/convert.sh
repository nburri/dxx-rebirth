#!/bin/sh
# How the bundled ships were made from the Quaternius packs (CC0 1.0):
#   Ultimate Spaceships  https://quaternius.com/packs/ultimatespaceships.html
#   Ultimate Space Kit   https://quaternius.com/packs/ultimatespacekit.html
# Usage: data/ships/convert.sh <shipconv> <ultimate-spaceships dir> <ultimate-space-kit dir>
# The colour zone of the Ultimate Spaceships is what differs between the
# texture a model embeds (Orange) and its Red variant; Rae's is its
# orange hull colour (the material accent after prep_flat.py).
set -eu
shipconv=$1 qus=$2 qusk=$3
out=$(dirname "$0")
common="--author Quaternius --licence CC0-1.0"
for ship in Striker Dispatcher Zenith Pancake Spitfire Executioner; do
	name=$(echo "$ship" | tr 'A-Z' 'a-z')
	"$shipconv" "$qus/$ship/glTF/$ship.gltf" -o "$out/$name.dxship" --name "$name" --title "$ship" $common \
		--source https://quaternius.com/packs/ultimatespaceships.html \
		--description "Quaternius Ultimate Spaceships, CC0" \
		--colour-variant "$qus/$ship/Textures/${ship}_Red.png"
done
# Rae is flat-coloured (a palette atlas); like the Kenney speeders it is
# textured by src/texture/ (python3 with numpy, scipy, Pillow and pygltflib).
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$out/src/texture/prep_flat.py" "$qusk/GLTF/Spaceship_RaeTheRedPanda.gltf" "$tmp/rae-flat.glb" rae > /dev/null
python3 "$out/src/texture/texture_ships.py" "$tmp/rae-flat.glb" "$tmp/rae.glb" rae > /dev/null
"$shipconv" "$tmp/rae.glb" -o "$out/rae.dxship" --name rae --title Rae --author "Quaternius; textures: Claude" --licence CC0-1.0 \
	--source https://quaternius.com/packs/ultimatespacekit.html \
	--description "Quaternius Ultimate Space Kit, CC0; textured by this fork (own scripts + Scenario AI tiles)"
