#!/bin/sh
# How the Cow was made: the Cow of Quaternius' Ultimate Animated Animal Pack
# (CC0 1.0, https://quaternius.com/packs/ultimateanimatedanimals.html),
# repainted and extended by src/cow.py (spots and collar are the colour zone).
# Usage: data/ships/convert-cow.sh <shipconv> <animal pack dir>   (python3 with numpy)
set -eu
shipconv=$1 pack=$2
out=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 "$out/src/cow.py" "$pack/glTF/Cow.gltf" "$tmp/cow.glb" > /dev/null
"$shipconv" "$tmp/cow.glb" -o "$out/cow.dxship" --name cow --title Cow \
	--author "Quaternius; paint, harness, guns: Claude" --licence CC0-1.0 \
	--source https://quaternius.com/packs/ultimateanimatedanimals.html \
	--description "Armed Holstein (Quaternius Ultimate Animated Animals, CC0): spots, collar and missile noses in your colour; rotary cannon under the chin, udder churn."
