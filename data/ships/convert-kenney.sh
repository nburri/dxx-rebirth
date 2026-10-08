#!/bin/sh
# How the Kenney ships were made from the Space Kit (CC0 1.0):
#   https://kenney.nl/assets/space-kit
# Usage: data/ships/convert-kenney.sh <shipconv> <space kit dir>
# The models are flat-coloured; src/texture/prep_flat.py sorts their faces into
# surface kinds (metalRed is the colour zone) and src/texture/texture_ships.py
# textures them like the fork's own ships (python3 with numpy, scipy, Pillow
# and pygltflib).
set -eu
shipconv=$1 kit=$2
out=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for v in C D; do
	name=speeder-$(echo "$v" | tr 'A-Z' 'a-z')
	python3 "$out/src/texture/prep_flat.py" "$kit/Models/OBJ format/craft_speeder$v.obj" "$tmp/$name-flat.glb" "$name" > /dev/null
	python3 "$out/src/texture/texture_ships.py" "$tmp/$name-flat.glb" "$tmp/$name.glb" "$name" > /dev/null
	"$shipconv" "$tmp/$name.glb" -o "$out/$name.dxship" --name "$name" --title "Speeder $v" \
		--author "Kenney; textures: Claude" --licence CC0-1.0 --source https://kenney.nl/assets/space-kit \
		--description "Kenney Space Kit craft_speeder$v, CC0; textured by this fork (own scripts + Scenario AI tiles)"
done
