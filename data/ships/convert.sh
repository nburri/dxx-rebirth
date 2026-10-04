#!/bin/sh
# How the bundled ships were made from the Quaternius packs (CC0 1.0):
#   Ultimate Spaceships  https://quaternius.com/packs/ultimatespaceships.html
#   Ultimate Space Kit   https://quaternius.com/packs/ultimatespacekit.html
# Usage: data/ships/convert.sh <shipconv> <ultimate-spaceships dir> <ultimate-space-kit dir>
# The colour zone of the Ultimate Spaceships is what differs between the
# texture a model embeds (Orange) and its Red variant; Rae's is its
# orange hull colour.
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
"$shipconv" "$qusk/GLTF/Spaceship_RaeTheRedPanda.gltf" -o "$out/rae.dxship" --name rae --title Rae $common \
	--source https://quaternius.com/packs/ultimatespacekit.html \
	--description "Quaternius Ultimate Space Kit, CC0" \
	--colour-key BD5C20:6
