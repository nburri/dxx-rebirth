# Bundled custom ships

Ships players can fly instead of the Pyro-GX (Options → Ship...;
`Documentation/custom-ships.md`). The release packages put them into the
game's `ships` folder.

| File | Ship | Author | Licence | Source |
|---|---|---|---|---|
| `striker.dxship` | Striker | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `dispatcher.dxship` | Dispatcher | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `zenith.dxship` | Zenith | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `pancake.dxship` | Pancake | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `spitfire.dxship` | Spitfire | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `executioner.dxship` | Executioner | Quaternius | CC0 1.0 | [Ultimate Spaceships](https://quaternius.com/packs/ultimatespaceships.html) |
| `rae.dxship` | Rae | Quaternius | CC0 1.0 | [Ultimate Space Kit](https://quaternius.com/packs/ultimatespacekit.html) |
| `speeder-c.dxship` | Speeder C | Kenney | CC0 1.0 | [Space Kit](https://kenney.nl/assets/space-kit) |
| `speeder-d.dxship` | Speeder D | Kenney | CC0 1.0 | [Space Kit](https://kenney.nl/assets/space-kit) |
| `anvil.dxship` | Anvil | this fork (procedural) | CC0 1.0 | `src/anvil.glb` |
| `manta.dxship` | Manta | this fork (procedural) | CC0 1.0 | `src/manta.glb` |
| `locust.dxship` | Locust | this fork (procedural) | CC0 1.0 | `src/locust.glb` |
| `bulwark.dxship` | Bulwark | this fork (procedural) | CC0 1.0 | `src/bulwark.glb` |
| `cow.dxship` | Cow | Quaternius; spots, collar, bell: this fork | CC0 1.0 | [Ultimate Animated Animal Pack](https://quaternius.com/packs/ultimateanimatedanimals.html), `src/cow.py` |

The models are public domain (CC0 1.0 Universal); credit to Quaternius
(https://quaternius.com) and Kenney (https://kenney.nl) all the same, and the same credit is in each
file's manifest. `convert.sh` and `convert-kenney.sh` show how each was converted with
`shipconv` from the packs' glTF files (the packs themselves are not in
this repository).

Anvil, Manta, Locust and Bulwark are the fork's own designs: compact,
wide ships of roughly the Pyro-GX's proportions, generated procedurally
(no third-party content), dedicated to the public domain (CC0 1.0). Their
glTF sources are in `src/`; `convert-own.sh` converts them.

Their textures (one 512 × 512 atlas per ship, also CC0 1.0) come from
`src/texture/texture_ships.py`, which unwraps the glTF sources and paints
the atlas: tileable hull tiles (`src/texture/tiles/`, made with Scenario's
"Scenario Texture" model from text prompts only, see `PROMPTS.txt` and
`LICENSE.txt` there) recoloured per ship, plus procedural detail (edge
creases and wear, soot, grime, hazard stripes, vents, hull numbers, glass,
glowing nozzles). The player-colour zone (material `accent`) is a
near-white painted plate; `shipconv` greys it and the game multiplies it
with the player's colour. `convert-own.sh` runs both steps.

The Cow is the Cow of Quaternius' Ultimate Animated Animal Pack (CC0 1.0,
its `License.txt` and the pack's page) in its rest pose, repainted by
`src/cow.py` as a Holstein: white hide, pink snout and udder, dark hooves;
its spots and a collar are the player-colour zone, and a brass cow bell
hangs at the neck. Head, legs, udder, tail and bell fly off as debris.
`convert-cow.sh` rebuilds it from the pack's `glTF/Cow.gltf`.
