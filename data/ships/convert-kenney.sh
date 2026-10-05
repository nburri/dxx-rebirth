#!/bin/sh
# How the Kenney ships were made from the Space Kit (CC0 1.0):
#   https://kenney.nl/assets/space-kit
# Usage: data/ships/convert-kenney.sh <shipconv> <space kit dir>
# The colour zone is the material metalRed.
set -eu
shipconv=$1 kit=$2
out=$(dirname "$0")
for v in C D; do
	name=speeder-$(echo "$v" | tr 'A-Z' 'a-z')
	"$shipconv" "$kit/Models/OBJ format/craft_speeder$v.obj" -o "$out/$name.dxship" --name "$name" --title "Speeder $v" \
		--author Kenney --licence CC0-1.0 --source https://kenney.nl/assets/space-kit \
		--description "Kenney Space Kit craft_speeder$v, CC0" --colour-material metalRed
done
